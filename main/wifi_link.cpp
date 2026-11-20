#include "wifi_link.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "card_files.hpp"
#include "dashboard_console.hpp"
#include "firmware_update.hpp"
#include "log_tee.hpp"
#include "ssh_console.hpp"
#include "emcool/log.hpp"
#include "emcool/settings/remote_console.hpp"
#include "emcool/text/help.hpp"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace emcool {

namespace {

constexpr const char* kTag = "wifi";
constexpr const char* kHostname = "emcool";
constexpr std::uint16_t kConsolePort = 23;
// Core 0 with NimBLE and the ride-log task; core 1 is the CAN decode task's.
constexpr BaseType_t kCore = 0;
constexpr UBaseType_t kConsolePriority = 2;
constexpr std::uint32_t kConsoleStackBytes = 6144;
// Waits between reconnect attempts, doubling from 1 s to a minute.
constexpr std::int64_t kFirstRetryMs = 1000;
constexpr std::int64_t kLastRetryMs = 60000;
// The web page's piece of a download: one /dl piece, sent as one HTTP chunk.
constexpr std::size_t kHttpPieceBytes = 4096;
// About a status report's worth of log, waiting for the console's client.
constexpr std::size_t kLogBufferBytes = 4096;

std::int64_t nowMs() { return esp_timer_get_time() / 1000; }

struct Link {
  WifiCounters* counters = nullptr;
  settings::SettingsStorage* storage = nullptr;
  settings::RuntimeSettings* running = nullptr;
  ride_log::DownloadCard* card = nullptr;
  std::string telnetPassword;
  std::string ssid;
  // Whether port 23 is served this boot (TELNET_CONSOLE, and a TELNET_PASSWORD to ask for), and
  // port 22 (ADMIN_PASSWORD).
  bool console = false;
  bool ssh = false;
  esp_netif_t* netif = nullptr;
  esp_timer_handle_t retryTimer = nullptr;
  std::int64_t retryMs = kFirstRetryMs;
  httpd_handle_t http = nullptr;

  // The telnet console's copy of the log while a client is logged in (log_tee.hpp).
  LogSink* log = nullptr;
} link;

// Sends everything, or gives up: the console's socket is blocking, with a send timeout.
bool sendAll(int socket, const char* data, std::size_t length) {
  while (length > 0) {
    const ssize_t sent = send(socket, data, length, 0);
    if (sent <= 0) return false;
    data += sent;
    length -= static_cast<std::size_t>(sent);
  }
  return true;
}

// What the log tee has buffered, to the client: from the console task only.
bool drainLog(int client) {
  std::string text;
  link.log->drain(text);
  if (text.empty()) return true;
  text = settings::withCrLf(text);
  return sendAll(client, text.data(), text.size());
}

// ---------------------------------------------------------------------------------- the console

// The telnet console's commands: the USB console's that only look (settings::readOnlyAllows). Its
// password crosses the network in plain text, so nothing here saves, deletes, restarts, installs or
// transmits; the SSH console does those.
// The card commands' state for the telnet console: its folder, kept from client to client unless reset.
std::unique_ptr<files::CardShell> cardShell;

void runCommand(int argumentCount, char** arguments, std::string& output) {
  const std::string_view command = arguments[0];
  if (!settings::readOnlyAllows(argumentCount, arguments)) {
    if (settings::isFullConsoleCommand(command)) {
      output += "`" + std::string(command) +
                "` isn't on the telnet console, which only looks: its password crosses the network in plain "
                "text. Use the SSH console (ssh admin@<bike>, ADMIN_PASSWORD) or the USB console.\n";
    } else {
      output += "no command called " + std::string(command) + "; `help` lists them\n";
    }
    return;
  }
  if (command == "update") {
    runUpdateCommand(argumentCount, arguments, cardFiles().card, [&](std::string& piece) { output += piece; });
    return;
  }
  if (files::CardShell::handles(command)) {
    if (cardFiles().card == nullptr) {
      output += kNoCardConfigured;
      return;
    }
    if (!cardShell) cardShell = std::make_unique<files::CardShell>(*cardFiles().card, cardFiles().gate, "telnet console", 16 * 1024);
    cardShell->run(argumentCount, arguments, [&](std::string& piece) { output += piece; });
    return;
  }
  if (command == "help") {
    static constexpr text::HelpLine kHelp[] = {
        {"", "The telnet console only looks; the SSH console (port 22) changes things."},
        {"", ""},
        {"show", "list every setting and its value"},
        {"status", "print the status report now"},
        {"ifconfig", "the Wi-Fi network, address and MAC address"},
        {"update status", "which firmware runs now, and which boots next"},
        {"dir [PATH]", "list a folder on the card (also ls)"},
        {"cd [FOLDER]", "change folder; cd alone goes to the top"},
        {"pwd", "show the folder you're in"},
        {"type FILE", "print a text file (also cat)"},
        {"exit", "leave this console (also quit, logout)"},
    };
    text::appendHelp(output, kHelp);
    return;
  }
  if (command == "ifconfig") {
    output += describeNetwork();
    return;
  }
  // show, and status with no arguments (a report now).
  settings::runSettingsCommand(*link.storage, *link.running, argumentCount, arguments, output);
}

// One client: its input, its replies, and the log between them, all from this task. A client that
// says nothing for kIdleMs is let go, so a forgotten `nc` doesn't hold the console for ever.
constexpr std::int64_t kIdleMs = 30 * 60 * 1000;
constexpr std::int64_t kLoginMs = 60 * 1000;

void serveConsoleClient(int client, settings::RemoteConsole& console) {
  timeval timeout = {};
  timeout.tv_sec = 5;
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  const int on = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  std::string out = console.connected(nowMs());
  if (cardShell) cardShell->reset();
  if (!sendAll(client, out.data(), out.size())) return;
  std::int64_t lastHeardMs = nowMs();
  bool wasAuthenticated = false;
  char buffer[256];
  while (true) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(client, &readable);
    timeval wait = {};
    wait.tv_usec = 100 * 1000;
    const int ready = select(client + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) break;
    if (ready > 0) {
      const ssize_t received = recv(client, buffer, sizeof buffer, 0);
      if (received <= 0) break;
      lastHeardMs = nowMs();
      bool close = false;
      out = console.receive(std::span<const char>(buffer, static_cast<std::size_t>(received)), nowMs(), close);
      if (console.authenticated() && !wasAuthenticated) {
        wasAuthenticated = true;
        link.counters->consoleLogins.fetch_add(1, std::memory_order_relaxed);
        logInfo(kTag, "console: logged in over Wi-Fi");
        link.log->start();
      }
      if (!sendAll(client, out.data(), out.size()) || close) break;
    }
    if (wasAuthenticated && !drainLog(client)) break;
    const std::int64_t quietMs = nowMs() - lastHeardMs;
    if (quietMs > (wasAuthenticated ? kIdleMs : kLoginMs)) {
      const char* note = "\r\nidle too long; bye\r\n";
      sendAll(client, note, std::strlen(note));
      break;
    }
  }
  link.log->stop();
  if (!wasAuthenticated) link.counters->consoleRefusals.fetch_add(1, std::memory_order_relaxed);
}

// One client at a time; a second is told so and closed.
void consoleTask(void*) {
  settings::RemoteConsole console(link.telnetPassword, runCommand);
  const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(kConsolePort);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  const int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      listen(listener, 1) != 0) {
    logError(kTag, "the console could not listen on port %u (errno %d)", kConsolePort, errno);
    vTaskDelete(nullptr);
    return;
  }
  while (true) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    link.counters->consoleClient.store(true, std::memory_order_relaxed);
    serveConsoleClient(client, console);
    shutdown(client, SHUT_RDWR);
    close(client);
    link.counters->consoleClient.store(false, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------------- the web page

// HTTP Basic, user name ignored: the browser asks once and remembers it for the session.
bool authorised(httpd_req_t* request) {
  char header[256] = {};
  if (httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof header) == ESP_OK) {
    const std::optional<std::string> password = settings::basicAuthPassword(header);
    if (password && settings::constantTimeEquals(*password, link.telnetPassword)) return true;
  }
  link.counters->pagesRefused.fetch_add(1, std::memory_order_relaxed);
  httpd_resp_set_status(request, "401 Unauthorized");
  httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"emcool\", charset=\"UTF-8\"");
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_sendstr(request, "emcool: the telnet password (TELNET_PASSWORD) is needed; any user name will do\n");
  return false;
}

std::string megabytes(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return text;
}

esp_err_t sendText(httpd_req_t* request, const char* status, const std::string& text) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_send(request, text.data(), static_cast<ssize_t>(text.size()));
}

// GET /: every boot and part, with links to download them.
esp_err_t servePage(httpd_req_t* request) {
  if (!authorised(request)) return ESP_OK;
  link.counters->pagesServed.fetch_add(1, std::memory_order_relaxed);
  if (link.card == nullptr) return sendText(request, "503 Service Unavailable", "emcool: no microSD card is configured\n");
  std::string problem;
  const std::optional<ride_log::CardListing> listing = ride_log::listCard(*link.card, problem);
  if (!listing) return sendText(request, "503 Service Unavailable", "emcool: " + problem + "\n");
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  std::string page =
      "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>emcool ride log</title>"
      "<style>body{font:15px system-ui;margin:1.5em;max-width:40em}td{padding:.15em .8em .15em 0}"
      "h2{font-size:1em;margin:1.4em 0 .3em}</style><h1>emcool ride log</h1><p>" +
      megabytes(listing->bytes) + " on the card. Each file imports with <code>node tools/import-ride-log.mts &lt;file&gt;</code>.</p>"
      "<p><a href='/dl?mb=16'>Recent ride (newest 16 MB)</a> · <a href='/dl?mb=256'>Newest 256 MB</a></p>";
  httpd_resp_send_chunk(request, page.data(), static_cast<ssize_t>(page.size()));
  for (const ride_log::CardListing::Boot& boot : listing->boots) {
    char line[256];
    std::snprintf(line, sizeof line, "<h2>Boot %05u · %s · <a href='/dl?boot=%u&amp;mb=256'>download</a></h2><table>",
                  static_cast<unsigned>(boot.boot), megabytes(boot.bytes).c_str(), static_cast<unsigned>(boot.boot));
    page = line;
    for (const ride_log::CardListing::Part& part : boot.parts) {
      std::snprintf(line, sizeof line, "<tr><td>PART%04u.LOG</td><td>%s</td><td>%s</td></tr>",
                    static_cast<unsigned>(part.part), megabytes(part.bytes).c_str(),
                    part.live ? "being written" : "");
      page += line;
    }
    page += "</table>";
    if (httpd_resp_send_chunk(request, page.data(), static_cast<ssize_t>(page.size())) != ESP_OK) return ESP_FAIL;
  }
  return httpd_resp_send_chunk(request, nullptr, 0);
}

// GET /dl[?mb=N][&boot=N]: the dashboard's download, with the same choice of parts.
esp_err_t serveDownload(httpd_req_t* request) {
  if (!authorised(request)) return ESP_OK;
  link.counters->pagesServed.fetch_add(1, std::memory_order_relaxed);
  if (link.card == nullptr) return sendText(request, "503 Service Unavailable", "emcool: no microSD card is configured\n");
  ride_log::DownloadSelection selection;
  char query[64] = {};
  char value[16] = {};
  if (httpd_req_get_url_query_str(request, query, sizeof query) == ESP_OK) {
    if (httpd_query_key_value(query, "mb", value, sizeof value) == ESP_OK) {
      const unsigned long megabytes = std::strtoul(value, nullptr, 10);
      if (megabytes < 1 || megabytes > ride_log::kMaxDownloadBytes / ride_log::kMegabyte) {
        return sendText(request, "400 Bad Request", "mb= wants a whole number of megabytes from 1 to 256\n");
      }
      selection.maxBytes = megabytes * ride_log::kMegabyte;
    }
    if (httpd_query_key_value(query, "boot", value, sizeof value) == ESP_OK) {
      const unsigned long boot = std::strtoul(value, nullptr, 10);
      if (boot < 1 || boot > 99999) return sendText(request, "400 Bad Request", "boot= wants a boot number\n");
      selection.boot = static_cast<std::uint32_t>(boot);
    }
  }
  // Its own download, so this and a Bluetooth one can run at once. The server runs one handler at
  // a time, so one is enough here.
  static std::unique_ptr<ride_log::LogDownload> download;
  if (!download) download = std::make_unique<ride_log::LogDownload>(*link.card);
  ride_log::LogDownload::Outcome outcome = download->start(selection);
  while (outcome == ride_log::LogDownload::Outcome::kMeasuring) outcome = download->measureNext();
  if (outcome != ride_log::LogDownload::Outcome::kReady) {
    const std::string problem = "emcool: " + download->problem() + "\n";
    download->stop();
    return sendText(request, outcome == ride_log::LogDownload::Outcome::kNothing ? "404 Not Found" : "503 Service Unavailable",
                    problem);
  }
  const std::string disposition = "attachment; filename=\"" + download->fileName() + "\"";
  const std::string length = std::to_string(download->totalBytes());
  httpd_resp_set_type(request, "application/octet-stream");
  httpd_resp_set_hdr(request, "Content-Disposition", disposition.c_str());
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  // Chunked, so a part that fails mid-read ends the response early — never a short file passed off
  // as whole: the client sees the transfer cut off without its final chunk.
  static char piece[kHttpPieceBytes];
  std::uint64_t offset = 0;
  while (offset < download->totalBytes()) {
    const std::size_t got = download->read(offset, std::span<char>(piece, sizeof piece));
    if (got == 0 || httpd_resp_send_chunk(request, piece, static_cast<ssize_t>(got)) != ESP_OK) {
      logWarning(kTag, "download cut short at %llu of %s bytes: %s", static_cast<unsigned long long>(offset),
                 length.c_str(), got == 0 ? download->problem().c_str() : "the client went away");
      download->stop();
      // Returning a failure makes the server drop the connection without the final chunk.
      return ESP_FAIL;
    }
    offset += got;
    link.counters->bytesDownloaded.fetch_add(got, std::memory_order_relaxed);
  }
  download->stop();
  return httpd_resp_send_chunk(request, nullptr, 0);
}

bool startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.core_id = kCore;
  config.stack_size = 6144;
  config.max_open_sockets = 3;
  config.lru_purge_enable = true;
  config.send_wait_timeout = 10;
  if (httpd_start(&link.http, &config) != ESP_OK) {
    logError(kTag, "the ride-log page did not start");
    return false;
  }
  const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = servePage, .user_ctx = nullptr};
  const httpd_uri_t download = {.uri = "/dl", .method = HTTP_GET, .handler = serveDownload, .user_ctx = nullptr};
  httpd_register_uri_handler(link.http, &page);
  httpd_register_uri_handler(link.http, &download);
  return true;
}

// ---------------------------------------------------------------------------------- Wi-Fi itself

std::string address(const esp_ip4_addr_t& ip);

// What the bike serves at `ip`, for the log: "ssh admin@1.2.3.4, ride log http://1.2.3.4/, telnet off".
std::string describeServices(const std::string& ip) {
  std::string out = link.ssh ? "ssh admin@" + ip : std::string("ssh off (no ADMIN_PASSWORD)");
  out += link.telnetPassword.empty() ? std::string(", ride log off (no TELNET_PASSWORD)") : ", ride log http://" + ip + "/";
  out += link.console ? ", telnet `nc " + ip + " " + std::to_string(kConsolePort) + "` (read-only)"
                      : std::string(", telnet off");
  return out;
}

void connectLater() {
  esp_timer_stop(link.retryTimer);
  esp_timer_start_once(link.retryTimer, static_cast<std::uint64_t>(link.retryMs) * 1000);
  link.retryMs = std::min(link.retryMs * 2, kLastRetryMs);
}

void onEvent(void*, esp_event_base_t base, std::int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool wasConnected = link.counters->connected.exchange(false, std::memory_order_relaxed);
    link.counters->address.store(0, std::memory_order_relaxed);
    const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
    if (wasConnected) {
      logWarning(kTag, "disconnected (reason %u); reconnecting", event->reason);
    }
    link.counters->reconnects.fetch_add(1, std::memory_order_relaxed);
    connectLater();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    link.counters->address.store(event->ip_info.ip.addr, std::memory_order_relaxed);
    link.counters->connected.store(true, std::memory_order_relaxed);
    link.retryMs = kFirstRetryMs;
    const std::string ip = address(event->ip_info.ip);
    logInfo(kTag, "connected as %s: %s", ip.c_str(), describeServices(ip).c_str());
  }
}

std::string address(const esp_ip4_addr_t& ip) {
  char text[16];
  std::snprintf(text, sizeof text, IPSTR, IP2STR(&ip));
  return text;
}

}  // namespace

std::string describeNetwork() {
  // The station's MAC is fixed in eFuse, so it is there with Wi-Fi off too.
  std::uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macLine[48];
  std::snprintf(macLine, sizeof macLine, "mac:      %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
  if (link.netif == nullptr) {
    std::string out = std::string("wifi:     off (set WIFI_SSID, and ADMIN_PASSWORD or TELNET_PASSWORD, then restart)\n") +
                      macLine;
    // The host key is made with the first SSH start, but it's printed whenever there is one.
    const std::string fingerprint = sshHostKeyFingerprint();
    if (fingerprint != kNoHostKey) out += "host key: " + fingerprint + "\n";
    return out;
  }
  std::string out;
  char line[160];
  wifi_ap_record_t network = {};
  if (esp_wifi_sta_get_ap_info(&network) == ESP_OK) {
    std::snprintf(line, sizeof line, "wifi:     joined, channel %u, signal %d dBm\nssid:     %s\n",
                  static_cast<unsigned>(network.primary), static_cast<int>(network.rssi),
                  reinterpret_cast<const char*>(network.ssid));
  } else {
    std::snprintf(line, sizeof line, "wifi:     not joined; trying (%u attempts so far)\nssid:     %s\n",
                  static_cast<unsigned>(link.counters->reconnects.load(std::memory_order_relaxed)), link.ssid.c_str());
  }
  out += line;
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(link.netif, &ip) != ESP_OK || ip.ip.addr == 0) {
    out += "address:  none yet (waiting for the network's DHCP)\n";
  } else {
    // The network itself: the address with the host's bits cleared, and the mask's length.
    esp_ip4_addr_t subnet = {};
    subnet.addr = ip.ip.addr & ip.netmask.addr;
    const int prefix = __builtin_popcount(ip.netmask.addr);
    out += "address:  " + address(ip.ip) + "\n";
    out += "network:  " + address(subnet) + "/" + std::to_string(prefix) + "\n";
    out += "netmask:  " + address(ip.netmask) + "\n";
    out += "gateway:  " + address(ip.gw) + "\n";
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(link.netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns.ip.u_addr.ip4.addr != 0) {
      out += "dns:      " + address(dns.ip.u_addr.ip4) + "\n";
    }
  }
  const char* hostname = nullptr;
  esp_netif_get_hostname(link.netif, &hostname);
  out += std::string("hostname: ") + (hostname != nullptr ? hostname : "-") + "\n";
  out += macLine;
  const std::string at = ip.ip.addr != 0 ? address(ip.ip) : std::string("<bike>");
  out += link.ssh ? "ssh:      ssh admin@" + at + "\n" : std::string("ssh:      off (set ADMIN_PASSWORD, then restart)\n");
  if (link.ssh) out += "host key: " + sshHostKeyFingerprint() + "\n";
  out += link.console ? "telnet:   nc " + at + " " + std::to_string(kConsolePort) + " (read-only)\n"
                      : std::string("telnet:   off (TELNET_CONSOLE 0, or no TELNET_PASSWORD)\n");
  out += link.telnetPassword.empty() ? std::string("ride log: off (no TELNET_PASSWORD)\n")
                                     : "ride log: http://" + at + "/\n";
  return out;
}

bool startWifi(const settings::Settings& settings, settings::SettingsStorage& storage,
               settings::RuntimeSettings& running, ride_log::DownloadCard* card, WifiCounters& counters) {
  if (!settings.wifiEnabled()) {
    logInfo(kTag, "off (%s)",
            settings.wifiSsid.empty() ? "WIFI_SSID isn't set" : "neither ADMIN_PASSWORD nor TELNET_PASSWORD is set");
    return false;
  }
  link.counters = &counters;
  link.storage = &storage;
  link.running = &running;
  link.card = card;
  link.telnetPassword = settings.telnetPassword;
  link.ssid = settings.wifiSsid;
  link.console = settings.telnetEnabled();
  link.ssh = settings.sshEnabled();

  if (esp_netif_init() != ESP_OK) {
    logError(kTag, "esp_netif_init failed; Wi-Fi stays off");
    return false;
  }
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
    logError(kTag, "no event loop (%s); Wi-Fi stays off", esp_err_to_name(loop));
    return false;
  }
  link.netif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(link.netif, kHostname);
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t result = esp_wifi_init(&init);
  if (result != ESP_OK) {
    logError(kTag, "esp_wifi_init failed (%s); Wi-Fi stays off", esp_err_to_name(result));
    return false;
  }
  // The network's name and password live in emcool's settings, not Wi-Fi's own NVS copy.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  const esp_timer_create_args_t retry = {.callback = [](void*) { esp_wifi_connect(); },
                                         .arg = nullptr,
                                         .dispatch_method = ESP_TIMER_TASK,
                                         .name = "wifi-retry",
                                         .skip_unhandled_events = true};
  esp_timer_create(&retry, &link.retryTimer);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onEvent, nullptr);

  wifi_config_t config = {};
  std::memcpy(config.sta.ssid, settings.wifiSsid.data(), std::min(settings.wifiSsid.size(), sizeof config.sta.ssid));
  std::memcpy(config.sta.password, settings.wifiPassword.data(),
              std::min(settings.wifiPassword.size(), sizeof config.sta.password - 1));
  // With a password, nothing weaker than WPA2; WPA3 where the network offers it.
  config.sta.threshold.authmode = settings.wifiPassword.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  config.sta.pmf_cfg.capable = true;
  config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &config);
  // Light sleep between beacons is what lets Bluetooth and Wi-Fi share the one radio.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  result = esp_wifi_start();
  if (result != ESP_OK) {
    logError(kTag, "esp_wifi_start failed (%s); Wi-Fi stays off", esp_err_to_name(result));
    return false;
  }
  logInfo(kTag, "joining \"%s\"%s; once it connects: %s", settings.wifiSsid.c_str(),
          settings.wifiPassword.empty() ? " (an open network)" : "", describeServices("<bike>").c_str());

  // With the console off, nothing listens on port 23, and the log isn't teed for it.
  if (link.console) {
    link.log = LogSink::create(kLogBufferBytes);
    if (link.log == nullptr) {
      logError(kTag, "no memory for the telnet console's log buffer; it stays off");
      link.console = false;
    } else if (xTaskCreatePinnedToCore(consoleTask, "wifi-console", kConsoleStackBytes, nullptr, kConsolePriority,
                                       nullptr, kCore) != pdPASS) {
      logError(kTag, "could not create the console task");
      link.console = false;
    }
  }
  if (link.ssh && !startSshConsole(storage, running, counters)) link.ssh = false;
  // The ride-log page asks for the telnet password; without one, port 80 stays closed too.
  if (!settings.telnetPassword.empty()) startWebServer();
  return true;
}

}  // namespace emcool
