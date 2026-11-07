#include "emcool/ble_server/console_endpoints.hpp"

#include <array>
#include <cmath>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/settings/dashboard_reply.hpp"
#include "emcool/settings/remote_console.hpp"

namespace emcool::ble_server {

namespace {

constexpr const char* kTag = "console";
constexpr const char* kJson = "application/json; charset=utf-8";
constexpr const char* kText = "text/plain; charset=utf-8";
constexpr std::size_t kMaxWords = 8;

using settings::jsonString;

const char* typeName(settings::SettingType type) {
  switch (type) {
    case settings::SettingType::kSecret:
      return "secret";
    case settings::SettingType::kText:
      return "text";
    case settings::SettingType::kFlag:
      return "flag";
    case settings::SettingType::kInteger:
      return "integer";
  }
  return "text";
}

// What a secret is shown as, and what anything else is: never a secret's characters.
std::string shownValue(const settings::SettingSpec& spec, const std::string& value) {
  if (spec.type == settings::SettingType::kSecret) return jsonString(settings::describeValue(spec, value));
  return jsonString(value);
}

ConsoleAnswer reply(int status, bool ok, const std::string& output, std::uint32_t revision, bool restarting = false) {
  std::string body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"output\":" + jsonString(output) +
                     ",\"revision\":" + std::to_string(revision) + ",\"restarting\":" + (restarting ? "true" : "false") +
                     "}";
  return {status, kJson, std::move(body)};
}

constexpr std::string_view kHelp =
    "show | set NAME VALUE | unset NAME | status [on | off | every SECONDS] | network | restart\n"
    "Quote a value with spaces: set WIFI_SSID \"garage wifi\". Most settings take effect after restart; "
    "the status report's apply at once.\n"
    "(new-ble-key only on the USB console; Connections → Dashboard Bluetooth makes a key here)\n";

}  // namespace

std::optional<ConsoleAnswer> ConsoleEndpoints::begin(std::string_view method, std::string_view route,
                                                     std::string_view header, std::string_view body) {
  abandon();
  if (route == "/console/status") {
    if (method != "GET") return ConsoleAnswer{405, kText, "use GET to read the status report\n"};
    waitingForStatus_ = true;
    statusAsCommand_ = false;
    return poll();
  }
  if (method == "GET") return settingsList();
  if (method != "POST") return ConsoleAnswer{405, kText, "use GET to read the settings and POST to run a command\n"};
  if (header != kHeaderValue) {
    // The reference's CSRF refusal shape, which tells the page to reload rather than retry.
    return ConsoleAnswer{403, kJson,
                         "{\"ok\":false,\"csrfRefusal\":true,\"message\":\"A command needs the x-emcool: console "
                         "header.\",\"reason\":\"A command needs the x-emcool: console header.\"}"};
  }
  return run(body);
}

std::optional<ConsoleAnswer> ConsoleEndpoints::poll() {
  if (!waitingForStatus_) return std::nullopt;
  const bool first = !askedForStatus_;
  askedForStatus_ = true;
  std::string report;
  if (!host_.statusReport(first, report)) return std::nullopt;
  waitingForStatus_ = false;
  askedForStatus_ = false;
  if (statusAsCommand_) return reply(200, true, report, running_.revision());
  return ConsoleAnswer{200, kText, report + "\n" + host_.network()};
}

ConsoleAnswer ConsoleEndpoints::settingsList() {
  std::string out = "{\"revision\":" + std::to_string(running_.revision()) + ",\"settings\":[";
  bool firstSetting = true;
  for (const settings::SettingSpec& spec : settings::settingSpecs()) {
    bool stored = false;
    const std::string saved = settings::savedValue(storage_, spec, stored);
    const std::string now = running_.valueOf(spec);
    if (!firstSetting) out += ",";
    firstSetting = false;
    out += "{\"name\":" + jsonString(spec.name) + ",\"type\":\"" + typeName(spec.type) + "\"";
    out += ",\"saved\":" + shownValue(spec, saved) + ",\"active\":" + shownValue(spec, now);
    out += std::string(",\"isDefault\":") + (stored ? "false" : "true");
    out += std::string(",\"pending\":") + (saved != now ? "true" : "false");
    out += std::string(",\"live\":") + (spec.live ? "true" : "false");
    out += std::string(",\"set\":") + (saved.empty() ? "false" : "true");
    out += ",\"defaultValue\":" + shownValue(spec, std::string(spec.defaultValue));
    out += ",\"minimum\":" + std::to_string(spec.minimum) + ",\"maximum\":" + std::to_string(spec.maximum);
    out += ",\"help\":" + jsonString(spec.help) + "}";
  }
  out += "]}";
  return {200, kJson, std::move(out)};
}

std::optional<std::string> ConsoleEndpoints::restartRefusal(const ble::JsonObjectReader& request) {
  const std::vector<std::string> blockers = host_.restartBlockers();
  if (!blockers.empty()) {
    std::string why = "not restarting now: ";
    for (std::size_t at = 0; at < blockers.size(); at++) why += (at > 0 ? "; " : "") + blockers[at];
    return why + "\n";
  }
  const settings::SettingSpec& keySpec = *settings::findSetting("BLE_SECRET");
  bool stored = false;
  const std::string savedKey = settings::savedValue(storage_, keySpec, stored);
  if (savedKey == running_.boot().bleSecret) return std::nullopt;
  if (savedKey.empty()) {
    return std::string("not restarting from here: BLE_SECRET is unset, so Bluetooth would be off after the restart "
                       "and this page could not reconnect. Restart from the USB or Wi-Fi console.\n");
  }
  const ble::JsonMember* key = request.find("bluetoothKey");
  if (key == nullptr || key->type != ble::JsonType::kString || settings::validate(keySpec, key->text) ||
      !settings::constantTimeEquals(settings::normalizeValue(keySpec, key->text), savedKey)) {
    return std::string("not restarting yet: a new Bluetooth key is saved, and this phone would need it to reconnect. "
                       "Paste the new key beside Restart (it is used only to check, and kept on this phone), or "
                       "restart from the USB or Wi-Fi console.\n");
  }
  return std::nullopt;
}

std::optional<ConsoleAnswer> ConsoleEndpoints::run(std::string_view body) {
  ble::JsonObjectReader reader;
  std::array<char, 1024> scratch = {};
  const std::uint32_t revisionNow = running_.revision();
  if (!reader.read(body, scratch)) return reply(400, false, "the body must be a JSON object\n", revisionNow);
  const ble::JsonMember* lineMember = reader.find("line");
  if (lineMember == nullptr || lineMember->type != ble::JsonType::kString) {
    return reply(400, false, "the body needs a \"line\": the command, as typed at emcool>\n", revisionNow);
  }
  const std::string_view line = lineMember->text;
  if (line.size() > kMaxLineBytes) {
    return reply(400, false, "that line is longer than " + std::to_string(kMaxLineBytes) + " characters\n",
                 revisionNow);
  }
  for (const char character : line) {
    if (character == '\n' || character == '\r') return reply(400, false, "one command per line\n", revisionNow);
  }
  std::vector<std::string> words = settings::splitWords(line);
  if (words.empty()) return reply(400, false, "type a command; `help` lists them\n", revisionNow);
  if (words.size() > kMaxWords) {
    return reply(400, false, "too many words; a command takes at most " + std::to_string(kMaxWords) + "\n",
                 revisionNow);
  }
  std::array<const char*, kMaxWords> arguments = {};
  for (std::size_t at = 0; at < words.size(); at++) arguments[at] = words[at].c_str();
  const int count = static_cast<int>(words.size());
  const std::string_view command = words[0];

  if (command == "help") return reply(200, true, std::string(kHelp), revisionNow);
  // A report now, as on the console, only here it comes back to the page rather than to the serial log.
  if (command == "status" && count == 1) {
    waitingForStatus_ = true;
    statusAsCommand_ = true;
    return poll();
  }
  if (command == "network") return reply(200, true, host_.network(), revisionNow);
  if (command == "new-ble-key") {
    return reply(400, false,
                 "new-ble-key runs only on the USB console. Here, Settings → Connections → Dashboard Bluetooth → New "
                 "key makes one, or `set BLE_SECRET <64 hex>` saves a key you made elsewhere.\n",
                 revisionNow);
  }
  if (command == "restart") {
    if (count != 1) return reply(400, false, "usage: restart\n", revisionNow);
    if (const std::optional<std::string> refusal = restartRefusal(reader)) {
      return reply(409, false, *refusal, revisionNow);
    }
    logWarning(kTag, "restart asked for from the dashboard");
    host_.restartSoon();
    return reply(200, true, "restarting; the dashboard reconnects in a few seconds\n", revisionNow, true);
  }
  if (command != "show" && command != "set" && command != "unset" && command != "status") {
    return reply(400, false, "no command called " + std::string(command) + "; `help` lists them\n", revisionNow);
  }

  std::string output;
  int status = 0;
  if (settings::commandChangesSettings(count, arguments.data())) {
    const ble::JsonMember* revision = reader.find("revision");
    if (revision == nullptr || revision->type != ble::JsonType::kNumber || revision->number < 0 ||
        revision->number != std::floor(revision->number) || revision->number > 4294967295.0) {
      return reply(400, false, "a command that saves needs the \"revision\" this page read; read the settings again\n",
                   revisionNow);
    }
    // The name only: the value may be a password.
    logInfo(kTag, "from the dashboard: %s %s", words[0].c_str(), count > 1 ? words[1].c_str() : "");
    status = settings::runSettingsCommandAt(storage_, running_, static_cast<std::uint32_t>(revision->number), count,
                                            arguments.data(), output);
  } else {
    status = settings::runSettingsCommand(storage_, running_, count, arguments.data(), output);
  }
  return reply(status == 0 ? 200 : status == 2 ? 409 : 400, status == 0, output, running_.revision());
}

}  // namespace emcool::ble_server
