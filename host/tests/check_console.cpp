// The dashboard's Bike console (/console, /console/status) against an in-memory store in place of
// NVS: what it lists, what it runs and refuses, the revision check, the status report, and the
// restart's guards. And, through DashboardEndpoints, that a request's body reaches it.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/ble_server/console_endpoints.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/files/card_shell.hpp"
#include "emcool/settings/settings.hpp"
#include "emcool/signals/signal_store.hpp"

using namespace emcool;
using ble_server::ConsoleAnswer;
using ble_server::ConsoleEndpoints;

namespace {

class MemoryStorage final : public settings::SettingsStorage {
 public:
  std::optional<std::string> read(std::string_view key) override {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return std::nullopt;
    return found->second;
  }
  bool write(std::string_view key, std::string_view value) override {
    values[std::string(key)] = std::string(value);
    return true;
  }
  bool erase(std::string_view key) override {
    values.erase(std::string(key));
    return true;
  }
  std::map<std::string, std::string> values;
};

class FakeHost final : public ble_server::ConsoleHost {
 public:
  bool statusReport(bool first, std::string& text) override {
    if (first) {
      asked++;
      looks = 0;
      return false;
    }
    if (++looks < 3) return false;
    text = "can: 812 frames/s\nlive: soc=81\n";
    return true;
  }
  std::string network() override { return "wifi: off\n"; }
  std::vector<std::string> restartBlockers() override { return blockers; }
  void restartSoon() override { restarts++; }

  int asked = 0;
  int looks = 0;
  int restarts = 0;
  std::vector<std::string> blockers;
};

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The reply's fields, read back as the dashboard reads them.
struct Run {
  int status;
  bool ok;
  std::string output;
  double revision;
  bool restarting;
  std::string body;
};

Run parse(const ConsoleAnswer& answer) {
  ble::JsonObjectReader reader;
  static char scratch[8192];
  Run run{answer.status, false, "", -1, false, answer.body};
  if (!reader.read(answer.body, scratch)) return run;
  if (const ble::JsonMember* ok = reader.find("ok")) run.ok = ok->text == "true";
  if (const ble::JsonMember* output = reader.find("output")) run.output = std::string(output->text);
  if (const ble::JsonMember* revision = reader.find("revision")) run.revision = revision->number;
  if (const ble::JsonMember* restarting = reader.find("restarting")) run.restarting = restarting->text == "true";
  return run;
}

std::string jsonText(std::string_view text) {
  std::string out = "\"";
  for (const char character : text) {
    if (character == '"' || character == '\\') out += '\\';
    out += character;
  }
  return out + "\"";
}

class Console {
 public:
  Console(MemoryStorage& storage, settings::RuntimeSettings& running)
      : running_(running), endpoints_(storage, running, host) {}

  Run post(std::string_view line, std::optional<std::uint32_t> revision, std::string_view extra = "",
           std::string_view header = ConsoleEndpoints::kHeaderValue) {
    std::string body = "{\"line\":" + jsonText(line);
    if (revision) body += ",\"revision\":" + std::to_string(*revision);
    body += extra;
    body += "}";
    return parse(answer("POST", "/console", header, body));
  }
  Run postNow(std::string_view line, std::string_view extra = "") { return post(line, running_.revision(), extra); }

  ConsoleAnswer answer(std::string_view method, std::string_view route, std::string_view header,
                       std::string_view body) {
    std::optional<ConsoleAnswer> got = endpoints_.begin(method, route, header, body);
    for (int poll = 0; !got && poll < 10; poll++) got = endpoints_.poll();
    EMCOOL_CHECK(got.has_value(), "%.*s %.*s never answered", static_cast<int>(method.size()), method.data(),
                 static_cast<int>(route.size()), route.data());
    return got.value_or(ConsoleAnswer{0, "", ""});
  }

  FakeHost host;

 private:
  settings::RuntimeSettings& running_;
  ConsoleEndpoints endpoints_;
};

const std::string kKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kNewKey = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

}  // namespace

int main() {
  MemoryStorage storage;
  storage.values["ble_secret"] = kKey;
  storage.values["admin_password"] = "a long admin phrase";
  settings::RuntimeSettings running(settings::Settings::load(storage));
  Console console(storage, running);

  // GET /console: every setting, its saved and running value, and no secret's characters.
  {
    const ConsoleAnswer list = console.answer("GET", "/console", "", "");
    EMCOOL_CHECK(list.status == 200 && std::string_view(list.contentType).starts_with("application/json"),
                 "GET /console: %d", list.status);
    for (const settings::SettingSpec& spec : settings::settingSpecs()) {
      EMCOOL_CHECK(contains(list.body, "\"name\":\"" + std::string(spec.name) + "\""), "GET /console has no %.*s",
                   static_cast<int>(spec.name.size()), spec.name.data());
    }
    EMCOOL_CHECK(!contains(list.body, kKey) && !contains(list.body, "a long admin phrase"),
                 "GET /console sent a secret back:\n%s", list.body.c_str());
    EMCOOL_CHECK(contains(list.body, "\"set (19 characters)\"") && contains(list.body, "\"revision\":0"),
                 "GET /console:\n%s", list.body.c_str());
    ble::JsonObjectReader reader;
    static char scratch[16384];
    EMCOOL_CHECK(reader.read(list.body, scratch) && reader.find("settings") != nullptr,
                 "GET /console is not a JSON object with settings");
  }

  // A command that saves: applied, saved in NVS, and the revision moves on.
  Run run = console.post("set OBD_ENABLED 1", 0);
  EMCOOL_CHECK(run.status == 200 && run.ok && storage.values["obd_enabled"] == "1" && run.revision == 1 &&
                   contains(run.output, "OBD_ENABLED saved") && contains(run.output, "after `restart`"),
               "set OBD_ENABLED 1: %d %s", run.status, run.body.c_str());
  EMCOOL_CHECK(contains(console.answer("GET", "/console", "", "").body,
                        "{\"name\":\"OBD_ENABLED\",\"type\":\"flag\",\"saved\":\"1\",\"active\":\"0\",\"isDefault\":false,"
                        "\"pending\":true"),
               "OBD_ENABLED is not listed as waiting for a restart");

  // A stale revision: refused whole, nothing saved, the current one sent back.
  run = console.post("set CUSTOM_BMS_CONFIG 1", 0);
  EMCOOL_CHECK(run.status == 409 && !run.ok && storage.values.count("custom_bms") == 0 && run.revision == 1 &&
                   contains(run.output, "changed since this page read them"),
               "a stale revision saved: %d %s", run.status, run.body.c_str());
  // Another console's save makes the page's revision stale too.
  {
    std::string output;
    const char* words[] = {"set", "BLE_NAME", "garage-bike"};
    settings::runSettingsCommand(storage, running, 3, words, output);
  }
  run = console.post("set CUSTOM_BMS_CONFIG 1", 1);
  EMCOOL_CHECK(run.status == 409 && storage.values.count("custom_bms") == 0, "a save over the serial console's: %s",
               run.body.c_str());
  run = console.postNow("set CUSTOM_BMS_CONFIG 1");
  EMCOOL_CHECK(run.ok && storage.values["custom_bms"] == "1" && run.revision == 3, "set at the current revision: %s",
               run.body.c_str());
  // No revision at all, on a command that saves.
  run = console.post("unset CUSTOM_BMS_CONFIG", std::nullopt);
  EMCOOL_CHECK(run.status == 400 && storage.values["custom_bms"] == "1" && contains(run.output, "revision"),
               "a save without a revision: %s", run.body.c_str());

  // The console's own checks, unchanged.
  run = console.postNow("set CUSTOM_BMS_CONFIG maybe");
  EMCOOL_CHECK(run.status == 400 && !run.ok && contains(run.output, "must be 1 or 0"), "a bad flag: %s",
               run.body.c_str());
  run = console.postNow("set WIFI_SSID \"garage wifi\"");
  EMCOOL_CHECK(run.ok && storage.values["wifi_ssid"] == "garage wifi", "a quoted value: %s", run.body.c_str());
  run = console.postNow("set WIFI_PASSWORD \"pass \\\"word\\\" 42\"");
  EMCOOL_CHECK(run.ok && storage.values["wifi_password"] == "pass \"word\" 42" && !contains(run.body, "word"),
               "a quoted, escaped password: %s", run.body.c_str());
  run = console.postNow("unset CUSTOM_BMS_CONFIG");
  EMCOOL_CHECK(run.ok && storage.values.count("custom_bms") == 0 && contains(run.output, "back to its default"),
               "unset: %s", run.body.c_str());
  run = console.postNow("set NOT_A_SETTING 1");
  EMCOOL_CHECK(run.status == 400 && contains(run.output, "no setting called"), "an unknown setting: %s",
               run.body.c_str());
  // Every writing switch is reachable, as at the console.
  run = console.postNow("set SERVICE_WRITE_ENABLED 1");
  EMCOOL_CHECK(run.ok && storage.values["service_write"] == "1", "SERVICE_WRITE_ENABLED: %s", run.body.c_str());
  run = console.postNow("set CHARGE_AUTO_ENABLED on");
  EMCOOL_CHECK(run.ok && storage.values["charge_auto"] == "1", "CHARGE_AUTO_ENABLED: %s", run.body.c_str());

  // Reads need no revision, and say what `show` says.
  run = console.post("show", std::nullopt);
  EMCOOL_CHECK(run.ok && contains(run.output, "OBD_ENABLED        1   <- takes effect after `restart` (running: 0)") &&
                   !contains(run.output, kKey) && !contains(run.output, "a long admin phrase"),
               "show:\n%s", run.output.c_str());
  run = console.post("help", std::nullopt);
  EMCOOL_CHECK(run.ok && contains(run.output, "set NAME VALUE"), "help: %s", run.body.c_str());
  run = console.post("network", std::nullopt);
  EMCOOL_CHECK(run.ok && run.output == "wifi: off\n", "network: %s", run.body.c_str());

  // The live settings, as `status on|off|every`, apply at once.
  run = console.postNow("status every 30");
  EMCOOL_CHECK(run.ok && running.statusIntervalSeconds() == 30 && storage.values["status_every_s"] == "30",
               "status every 30: %s", run.body.c_str());
  run = console.postNow("status off");
  EMCOOL_CHECK(run.ok && !running.statusReport(), "status off: %s", run.body.c_str());
  // A typed `status` is the report itself, sent back rather than printed on the serial port.
  const int askedBefore = console.host.asked;
  run = console.post("status", std::nullopt);
  EMCOOL_CHECK(run.ok && run.output == "can: 812 frames/s\nlive: soc=81\n" && console.host.asked == askedBefore + 1 &&
                   !running.takeReportRequest(),
               "status: %s", run.body.c_str());
  // GET /console/status: the report and the network, as text.
  {
    const ConsoleAnswer status = console.answer("GET", "/console/status", "", "");
    EMCOOL_CHECK(status.status == 200 && status.body == "can: 812 frames/s\nlive: soc=81\n\nwifi: off\n" &&
                     std::string_view(status.contentType).starts_with("text/plain"),
                 "GET /console/status: %d %s", status.status, status.body.c_str());
    EMCOOL_CHECK(console.answer("POST", "/console/status", "console", "{}").status == 405,
                 "POST /console/status was not refused");
  }

  // new-ble-key is gone: an unknown command, and the key stays as it was.
  run = console.postNow("new-ble-key replace");
  EMCOOL_CHECK(run.status == 400 && storage.values["ble_secret"] == kKey && contains(run.output, "no command called"),
               "new-ble-key over the dashboard: %s", run.body.c_str());

  // Refusals: the header, the method, the body.
  run = console.post("show", std::nullopt, "", "");
  EMCOOL_CHECK(run.status == 403 && contains(run.body, "\"csrfRefusal\":true"), "a POST with no header: %s",
               run.body.c_str());
  run = console.post("show", std::nullopt, "", "service-mode");
  EMCOOL_CHECK(run.status == 403, "a POST with another request's header: %s", run.body.c_str());
  EMCOOL_CHECK(console.answer("DELETE", "/console", "console", "").status == 405, "DELETE /console");
  EMCOOL_CHECK(parse(console.answer("POST", "/console", "console", "not json")).status == 400, "a body that isn't JSON");
  EMCOOL_CHECK(parse(console.answer("POST", "/console", "console", "{\"line\":7}")).status == 400, "a line that isn't text");
  EMCOOL_CHECK(console.post("show\nset OBD_ENABLED 0", std::nullopt).status == 400 && storage.values["obd_enabled"] == "1",
               "two lines in one ran");
  EMCOOL_CHECK(console.post(std::string(ConsoleEndpoints::kMaxLineBytes + 1, 'x'), std::nullopt).status == 400,
               "an overlong line ran");
  EMCOOL_CHECK(console.post("set a b c d e f g h i", std::nullopt).status == 400, "nine words ran");
  EMCOOL_CHECK(console.post("", std::nullopt).status == 400, "an empty line");
  EMCOOL_CHECK(console.post("reboot", std::nullopt).status == 400, "an unknown command");

  // restart: refused while something blocks it, and with a new key it isn't given.
  console.host.blockers = {"wait until road speed is zero"};
  run = console.post("restart", std::nullopt);
  EMCOOL_CHECK(run.status == 409 && !run.restarting && console.host.restarts == 0 && contains(run.output, "road speed"),
               "restart while moving: %s", run.body.c_str());
  console.host.blockers.clear();
  run = console.post("restart now", std::nullopt);
  EMCOOL_CHECK(run.status == 400 && console.host.restarts == 0, "restart with words after it: %s", run.body.c_str());
  run = console.postNow("set BLE_SECRET " + kNewKey);
  EMCOOL_CHECK(run.ok && storage.values["ble_secret"] == kNewKey && !contains(run.body, kNewKey),
               "a typed new key: %s", run.body.c_str());
  run = console.post("restart", std::nullopt);
  EMCOOL_CHECK(run.status == 409 && console.host.restarts == 0 && contains(run.output, "new Bluetooth key"),
               "restart into a key this phone wasn't given: %s", run.body.c_str());
  run = console.post("restart", std::nullopt, ",\"bluetoothKey\":\"" + kKey + "\"");
  EMCOOL_CHECK(run.status == 409 && console.host.restarts == 0, "restart with the old key: %s", run.body.c_str());
  run = console.post("restart", std::nullopt, ",\"bluetoothKey\":\"" + kNewKey + "\"");
  EMCOOL_CHECK(run.status == 200 && run.ok && run.restarting && console.host.restarts == 1,
               "restart with the new key: %s", run.body.c_str());
  // With the key unset, this page could never reconnect: the USB or Wi-Fi console restarts instead.
  run = console.postNow("unset BLE_SECRET");
  EMCOOL_CHECK(run.ok, "unset BLE_SECRET: %s", run.body.c_str());
  run = console.post("restart", std::nullopt, ",\"bluetoothKey\":\"" + kNewKey + "\"");
  EMCOOL_CHECK(run.status == 409 && console.host.restarts == 1 && contains(run.output, "USB or Wi-Fi console"),
               "restart into Bluetooth off: %s", run.body.c_str());
  run = console.postNow("set BLE_SECRET " + kKey);
  run = console.post("restart", std::nullopt);
  EMCOOL_CHECK(run.status == 200 && run.restarting && console.host.restarts == 2,
               "restart with the running key saved again: %s", run.body.c_str());
  // A typed key: saved as typed, and Restart needs it exactly, case and all.
  run = console.postNow("set BLE_SECRET Bike2024key");
  EMCOOL_CHECK(run.ok && storage.values["ble_secret"] == "Bike2024key" && !contains(run.body, "Bike2024key"),
               "a typed key: %s", run.body.c_str());
  run = console.postNow("set BLE_SECRET bikekey");
  EMCOOL_CHECK(!run.ok && storage.values["ble_secret"] == "Bike2024key" && contains(run.output, "letter and one digit"),
               "a key with no digit: %s", run.body.c_str());
  run = console.post("restart", std::nullopt, ",\"bluetoothKey\":\"bike2024key\"");
  EMCOOL_CHECK(run.status == 409 && console.host.restarts == 2, "restart with the typed key in the wrong case: %s",
               run.body.c_str());
  run = console.post("restart", std::nullopt, ",\"bluetoothKey\":\"Bike2024key\"");
  EMCOOL_CHECK(run.status == 200 && run.restarting && console.host.restarts == 3, "restart with the typed key: %s",
               run.body.c_str());
  run = console.postNow("set BLE_SECRET " + kKey);

  // Through DashboardEndpoints: the body reaches /console, and the status report is waited for.
  {
    class Silent final : public signals::RideLogSink {
     public:
      void appendReading(std::int64_t, std::int64_t, const signals::SignalDef&, double) override {}
    } rideLog;
    signals::SignalStore store(rideLog);
    std::mutex storeMutex;
    class NoStorage final : public ble_server::StorageReport {
     public:
      ble_server::StorageStatus storage() override { return {false, 0, 0, std::nullopt, std::nullopt}; }
    } noStorage;
    class NoCodes final : public ble_server::TroubleCodeSource {
     public:
      obd::StoredCodes troubleCodes() override { return {}; }
    } noCodes;
    FakeHost host;
    ConsoleEndpoints consoleEndpoints(storage, running, host);
    ble_server::DashboardEndpoints endpoints(store, storeMutex, noStorage, noCodes, nullptr, nullptr, "{}",
                                             &consoleEndpoints);
    const auto body = [&](std::size_t length) {
      std::string text;
      while (text.size() < length) {
        const std::span<const char> piece = endpoints.body(text.size(), length - text.size());
        if (piece.empty()) break;
        text.append(piece.data(), piece.size());
      }
      return text;
    };
    const std::string request =
        "{\"line\":\"set BLE_NAME tunnelled\",\"revision\":" + std::to_string(running.revision()) + "}";
    std::optional<ble_server::HttpResponse> response =
        endpoints.begin({"POST", "/console", "console", request}, 0);
    EMCOOL_CHECK(response && response->status == 200 && storage.values["ble_name"] == "tunnelled" &&
                     contains(body(response->bodyLength), "BLE_NAME saved"),
                 "POST /console through the endpoints");
    response = endpoints.begin({"GET", "/console/status", "", ""}, 0);
    EMCOOL_CHECK(!response, "/console/status answered before the report was written");
    for (int poll = 0; !response && poll < 10; poll++) response = endpoints.poll(0);
    EMCOOL_CHECK(response && response->status == 200 && contains(body(response->bodyLength), "live: soc=81"),
                 "/console/status through the endpoints");
    // A page abandoned mid-wait doesn't leave the next request answered with the old report.
    response = endpoints.begin({"GET", "/console/status", "", ""}, 0);
    endpoints.abandon();
    response = endpoints.begin({"GET", "/console", "", ""}, 0);
    EMCOOL_CHECK(response && response->status == 200 && contains(body(response->bodyLength), "\"settings\":["),
                 "GET /console after an abandoned status");
    // With no console, /console is a 404 like any endpoint emcool doesn't have.
    ble_server::DashboardEndpoints without(store, storeMutex, noStorage, noCodes, nullptr, nullptr, "{}");
    response = without.begin({"GET", "/console", "", ""}, 0);
    EMCOOL_CHECK(response && response->status == 404, "/console without a console: %d", response ? response->status : 0);
  }

  // --- The card's commands from the Bike console: run in slices across polls, and stopped when the
  // phone gives up.
  {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("emcool-console-card-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "RIDES/00001");
    std::ofstream(root / "NOTES.TXT") << "hello from the card\n";
    std::ofstream(root / "BIG.BIN", std::ios::binary) << std::string(300000, 'b');
    class TestCard final : public files::Card {
     public:
      explicit TestCard(std::string root) : root_(std::move(root)) {}
      const std::string& mountPoint() const override { return root_; }
      std::mutex& lock() override { return mutex_; }
      files::CardState state() override { return {true, 1}; }
      files::CardFacts facts() override { return {1, false}; }
      std::optional<files::FreeSpace> freeSpace() override { return files::FreeSpace{1ull << 30, 2ull << 30}; }
      void changed(bool) override {}

     private:
      std::string root_;
      std::mutex mutex_;
    } card(root.string());
    files::CardShellGate gate;
    files::CardShell shell(card, gate, "Bike console", ConsoleEndpoints::kMaxOutputBytes);
    shell.setKeepFreeBytes(0);
    shell.setMaxOperationsPerStep(4);
    MemoryStorage storage;
    settings::RuntimeSettings running(settings::Settings::load(storage));
    FakeHost host;
    ConsoleEndpoints endpoints(storage, running, host, &shell);
    const auto post = [&](std::string_view line, int maxPolls, int& polls) {
      const std::string body = "{\"line\":" + jsonText(line) + "}";
      std::optional<ConsoleAnswer> got = endpoints.begin("POST", "/console", "console", body);
      polls = 0;
      while (!got && polls < maxPolls) {
        got = endpoints.poll();
        polls++;
      }
      return got;
    };
    int polls = 0;
    std::optional<ConsoleAnswer> got = post("type NOTES.TXT", 0, polls);
    Run r = got ? parse(*got) : Run{};
    EMCOOL_CHECK(got && r.status == 200 && r.ok && contains(r.output, "hello from the card"), "type over /console: %s",
                 got ? got->body.c_str() : "(none)");
    got = post("help", 0, polls);
    EMCOOL_CHECK(got && contains(parse(*got).output, "rm [-r] PATH"), "help lists the card's commands");
    // A copy of 300 KB takes many polls; the link task is never held for the whole of it.
    got = post("cp BIG.BIN COPY.BIN", 10000, polls);
    r = got ? parse(*got) : Run{};
    EMCOOL_CHECK(got && r.ok && polls > 3 && fs::file_size(root / "COPY.BIN") == 300000, "cp in slices: %d polls, %s",
                 polls, r.output.c_str());
    got = post("rm RIDES/00001", 0, polls);
    EMCOOL_CHECK(got && got->status == 400 && contains(parse(*got).output, "being recorded"), "the live ride from the phone: %s",
                 got ? got->body.c_str() : "(none)");
    // The phone gives up mid-copy: the copy stops and the part-made file goes.
    got = post("cp BIG.BIN COPY2.BIN", 1, polls);
    EMCOOL_CHECK(!got, "still copying after one poll");
    endpoints.abandon();
    EMCOOL_CHECK(!fs::exists(root / "COPY2.BIN") && !shell.running(), "abandon removes the part-made copy");
    got = post("dir", 0, polls);
    EMCOOL_CHECK(got && parse(*got).ok && contains(parse(*got).output, "COPY.BIN"), "and the next command runs: %s",
                 got ? got->body.c_str() : "(none)");
    // Without a card, a card command says so.
    ConsoleEndpoints cardless(storage, running, host);
    got = cardless.begin("POST", "/console", "console", "{\"line\":\"dir\"}");
    EMCOOL_CHECK(got && got->status == 503 && contains(parse(*got).output, "no card"), "no card from the phone");
    fs::remove_all(root);
  }

  std::printf("check_console: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
