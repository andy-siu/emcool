// The settings and the serial console's commands, against an in-memory store in place of NVS.

#include <cctype>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "emcool/log.hpp"
#include "emcool/settings/remote_console.hpp"
#include "emcool/settings/settings.hpp"

using namespace emcool::settings;

namespace {

class MemoryStorage final : public SettingsStorage {
 public:
  std::optional<std::string> read(std::string_view key) override {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return std::nullopt;
    return found->second;
  }
  bool write(std::string_view key, std::string_view value) override {
    if (failWrites) return false;
    values[std::string(key)] = std::string(value);
    return true;
  }
  bool erase(std::string_view key) override {
    values.erase(std::string(key));
    return true;
  }
  std::map<std::string, std::string> values;
  bool failWrites = false;
};

struct Result {
  int status;
  std::string output;
};

Result run(MemoryStorage& storage, RuntimeSettings& running, std::vector<const char*> arguments) {
  Result result{0, ""};
  result.status =
      runSettingsCommand(storage, running, static_cast<int>(arguments.size()), arguments.data(), result.output);
  return result;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

int main() {
  MemoryStorage storage;
  RuntimeSettings booted(Settings::load(storage));
  EMCOOL_CHECK(booted.boot().bleSecret.empty() && booted.boot().bleName == "emcool" && !booted.boot().customBmsConfig &&
                   booted.statusReport() && booted.statusIntervalSeconds() == 10,
               "the defaults are wrong");

  Result result = run(storage, booted, {"show"});
  EMCOOL_CHECK(result.status == 0 && contains(result.output, "BLE_SECRET") && contains(result.output, "not set") &&
                   contains(result.output, "CUSTOM_BMS_CONFIG  0 (default)") && !contains(result.output, "restart"),
               "show on a fresh store:\n%s", result.output.c_str());

  // The Bluetooth key: generated, or typed as 8-64 characters with a letter and a digit and no
  // spaces, quotes or backslashes; never echoed by `show`, not even its length.
  for (const char* rejected : {"correct horse battery staple", "bike 2024", "bike202", "abcdefgh", "12345678",
                               "bike\"2024", "bike\\2024", "bike\t2024", "p\xc3\xa4ss2024", "",
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"}) {
    result = run(storage, booted, {"set", "BLE_SECRET", rejected});
    EMCOOL_CHECK(result.status == 1 && contains(result.output, "letter and one digit") &&
                     contains(result.output, "openssl rand -hex 32") && storage.values.count("ble_secret") == 0,
                 "BLE_SECRET %s was accepted: %s", rejected, result.output.c_str());
  }
  for (const char* typed : {"bike2024", "Bike2024", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde",
                            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", "r1de-0n!{~}"}) {
    result = run(storage, booted, {"set", "BLE_SECRET", typed});
    EMCOOL_CHECK(result.status == 0 && storage.values["ble_secret"] == typed, "the typed key %s: %s", typed,
                 result.output.c_str());
    EMCOOL_CHECK(Settings::load(storage).bleSecret == typed, "the typed key %s did not load as typed", typed);
    result = run(storage, booted, {"show"});
    EMCOOL_CHECK(contains(result.output, "BLE_SECRET         set   <-") && !contains(result.output, typed),
                 "show with a typed key:\n%s", result.output.c_str());
  }
  storage.erase("ble_secret");
  const std::string generated = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  const std::string secret = "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
  EMCOOL_CHECK(run(storage, booted, {"new-ble-key"}).status == 1 && storage.values.count("ble_secret") == 0,
               "new-ble-key is still a command");

  // Bluetooth turns on the moment BLE_SECRET is set, once per boot, with the key as saved.
  {
    MemoryStorage fresh;
    RuntimeSettings running(Settings::load(fresh));
    std::vector<std::string> started;
    running.setBluetoothStarter(
        [&](const std::string& key) {
          started.push_back(key);
          return true;
        },
        false);
    EMCOOL_CHECK(!running.bluetoothOn(), "Bluetooth is on before a key is set");
    result = run(fresh, running, {"set", "BLE_SECRET", "bike 2024"});
    EMCOOL_CHECK(result.status == 1 && started.empty() && !running.bluetoothOn(), "a refused key started Bluetooth");
    std::string upper = generated;
    for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    result = run(fresh, running, {"set", "BLE_SECRET", upper.c_str()});
    EMCOOL_CHECK(result.status == 0 && started.size() == 1 && started[0] == generated && running.bluetoothOn() &&
                     contains(result.output, "Bluetooth is on now") && !contains(result.output, generated),
                 "set BLE_SECRET did not start Bluetooth with the saved key: %s", result.output.c_str());
    result = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(result.output, "BLE_SECRET         set") && !contains(result.output, "BLE_SECRET         set   <-"),
                 "show waits for a restart that Bluetooth no longer needs:\n%s", result.output.c_str());
    // On now, so another key waits for the restart, and the phone keeps the running one.
    result = run(fresh, running, {"set", "BLE_SECRET", secret.c_str()});
    EMCOOL_CHECK(result.status == 0 && started.size() == 1 && contains(result.output, "after `restart`") &&
                     running.valueOf(*findSetting("BLE_SECRET")) == generated,
                 "a second key started Bluetooth again: %s", result.output.c_str());
    result = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(result.output, "BLE_SECRET         set   <- takes effect after `restart`"),
                 "show did not mark the second key as waiting:\n%s", result.output.c_str());
  }
  // A start that fails says so, saves the key all the same, and isn't tried again before the restart.
  {
    MemoryStorage fresh;
    RuntimeSettings running(Settings::load(fresh));
    int attempts = 0;
    running.setBluetoothStarter(
        [&](const std::string&) {
          attempts++;
          return false;
        },
        false);
    result = run(fresh, running, {"set", "BLE_SECRET", "Bike2024key"});
    EMCOOL_CHECK(result.status == 0 && attempts == 1 && !running.bluetoothOn() && fresh.values["ble_secret"] == "Bike2024key" &&
                     contains(result.output, "did not start"),
                 "a failed start: %s", result.output.c_str());
    result = run(fresh, running, {"set", "BLE_SECRET", "Bike2025key"});
    EMCOOL_CHECK(result.status == 0 && attempts == 1 && contains(result.output, "after `restart`"),
                 "a failed start was tried again: %s", result.output.c_str());
  }
  // Bluetooth already on from boot: a key is saved for the restart, and nothing starts.
  {
    MemoryStorage fresh;
    fresh.values["ble_secret"] = generated;
    RuntimeSettings running(Settings::load(fresh));
    running.setBluetoothStarter(nullptr, true);
    EMCOOL_CHECK(running.bluetoothOn(), "Bluetooth from boot is not on");
    result = run(fresh, running, {"set", "BLE_SECRET", secret.c_str()});
    EMCOOL_CHECK(result.status == 0 && contains(result.output, "after `restart`") &&
                     running.valueOf(*findSetting("BLE_SECRET")) == generated,
                 "a new key with Bluetooth on: %s", result.output.c_str());
  }

  result = run(storage, booted, {"set", "BLE_SECRET", secret.c_str()});
  EMCOOL_CHECK(result.status == 0 && storage.values["ble_secret"] == secret, "set BLE_SECRET: %s", result.output.c_str());
  result = run(storage, booted, {"show"});
  EMCOOL_CHECK(!contains(result.output, secret.substr(0, 16)) && contains(result.output, "BLE_SECRET         set") &&
                   contains(result.output, "takes effect after `restart`"),
               "show after setting a key:\n%s", result.output.c_str());
  // A generated key pasted in upper case is the same key, as the dashboard treats it; a typed key
  // keeps its case (above).
  std::string upper = generated;
  for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  result = run(storage, booted, {"set", "BLE_SECRET", upper.c_str()});
  EMCOOL_CHECK(result.status == 0 && storage.values["ble_secret"] == generated, "an upper-case key: %s",
               result.output.c_str());
  result = run(storage, booted, {"set", "BLE_SECRET", secret.c_str()});
  EMCOOL_CHECK(result.status == 0 && storage.values["ble_secret"] == secret, "setting the key back: %s",
               result.output.c_str());

  // CUSTOM_BMS_CONFIG: any spelling of a flag, stored as 1 or 0, under a 15-character NVS key.
  result = run(storage, booted, {"set", "custom_bms_config", "on"});
  EMCOOL_CHECK(result.status == 0 && storage.values["custom_bms"] == "1", "set custom_bms_config on: %s",
               result.output.c_str());
  result = run(storage, booted, {"set", "CUSTOM_BMS_CONFIG", "maybe"});
  EMCOOL_CHECK(result.status == 1 && storage.values["custom_bms"] == "1", "a non-flag value was accepted");
  for (const SettingSpec& spec : settingSpecs()) {
    EMCOOL_CHECK(spec.storageKey.size() <= 15, "%.*s's NVS key is too long", static_cast<int>(spec.name.size()),
                 spec.name.data());
  }

  RuntimeSettings rebooted(Settings::load(storage));
  EMCOOL_CHECK(rebooted.boot().bleSecret == secret && rebooted.boot().customBmsConfig,
               "the stored settings did not load");
  result = run(storage, rebooted, {"show"});
  EMCOOL_CHECK(!contains(result.output, "restart"), "show still asked for a restart after one:\n%s",
               result.output.c_str());

  // BLE_NAME limits, unknown names, unset, and a failing store.
  EMCOOL_CHECK(run(storage, rebooted, {"set", "BLE_NAME", "this name is far too long for one"}).status == 1,
               "a 33-character name was accepted");
  EMCOOL_CHECK(run(storage, rebooted, {"set", "BLE_NAME", "bad\tname"}).status == 1, "a control character was accepted");
  EMCOOL_CHECK(run(storage, rebooted, {"set", "WIFI_ENABLED", "1"}).status == 1, "an unknown setting was accepted");
  result = run(storage, rebooted, {"unset", "BLE_SECRET"});
  EMCOOL_CHECK(result.status == 0 && storage.values.count("ble_secret") == 0, "unset did not remove the secret");
  storage.failWrites = true;
  result = run(storage, rebooted, {"set", "BLE_NAME", "bike"});
  EMCOOL_CHECK(result.status == 1 && contains(result.output, "could not save"), "a failed write was reported as saved");
  storage.failWrites = false;

  // A stored value that no longer validates falls back to the default, loudly.
  storage.values["ble_secret"] = "short";
  const auto warningsBefore = emcool::logCountsFor("settings").warning;
  const Settings fallback = Settings::load(storage);
  EMCOOL_CHECK(fallback.bleSecret.empty() && emcool::logCountsFor("settings").warning == warningsBefore + 1,
               "an invalid stored secret was used or not warned about");

  // Transmitting is off until asked for, and waits for a restart like any boot setting.
  {
    MemoryStorage fresh;
    RuntimeSettings running(Settings::load(fresh));
    EMCOOL_CHECK(!running.boot().obdEnabled && !running.boot().elockEnabled && !running.boot().serviceModeEnabled,
                 "a transmitting feature is on by default");
    Result shown = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(shown.output, "OBD_ENABLED        0 (default)") &&
                     contains(shown.output, "ELOCK_ENABLED      0 (default)") &&
                     contains(shown.output, "SERVICE_MODE_ENABLED 0 (default)"),
                 "show on a fresh store:\n%s", shown.output.c_str());
    shown = run(fresh, running, {"set", "obd_enabled", "on"});
    EMCOOL_CHECK(shown.status == 0 && fresh.values["obd_enabled"] == "1" && contains(shown.output, "after `restart`") &&
                     !running.boot().obdEnabled,
                 "set OBD_ENABLED on:\n%s", shown.output.c_str());
    shown = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(shown.output, "OBD_ENABLED        1   <- takes effect after `restart` (running: 0)"),
                 "show with OBD waiting for a restart:\n%s", shown.output.c_str());
    run(fresh, running, {"set", "ELOCK_ENABLED", "yes"});
    const Settings restarted = Settings::load(fresh);
    EMCOOL_CHECK(restarted.obdEnabled && restarted.elockEnabled, "OBD_ENABLED and ELOCK_ENABLED did not load");
  }

  // The status report: live settings, applied at once and saved; `status` asks for a report now.
  storage.erase("ble_secret");
  RuntimeSettings live(Settings::load(storage));
  result = run(storage, live, {"status", "off"});
  EMCOOL_CHECK(result.status == 0 && !live.statusReport() && storage.values["status_report"] == "0" &&
                   contains(result.output, "in effect now") && contains(result.output, "status report: off"),
               "status off:\n%s", result.output.c_str());
  result = run(storage, live, {"show"});
  EMCOOL_CHECK(!contains(result.output, "restart"), "show asked for a restart for a live setting:\n%s",
               result.output.c_str());
  result = run(storage, live, {"status", "every", "30"});
  EMCOOL_CHECK(result.status == 0 && live.statusReport() && live.statusIntervalSeconds() == 30 &&
                   storage.values["status_every_s"] == "30" && storage.values["status_report"] == "1" &&
                   contains(result.output, "every 30 s"),
               "status every 30:\n%s", result.output.c_str());
  EMCOOL_CHECK(run(storage, live, {"status", "every", "3"}).status == 1 && live.statusIntervalSeconds() == 30,
               "a 3 s interval was accepted");
  EMCOOL_CHECK(run(storage, live, {"status", "every", "ten"}).status == 1, "a non-number interval was accepted");
  EMCOOL_CHECK(run(storage, live, {"status", "sideways"}).status == 1, "a nonsense status mode was accepted");
  result = run(storage, live, {"set", "status_interval_s", "60"});
  EMCOOL_CHECK(result.status == 0 && live.statusIntervalSeconds() == 60, "set STATUS_INTERVAL_S did not apply");
  EMCOOL_CHECK(!live.takeReportRequest(), "a report was requested before `status`");
  result = run(storage, live, {"status"});
  EMCOOL_CHECK(result.status == 0 && live.takeReportRequest() && !live.takeReportRequest() &&
                   contains(result.output, "every 60 s"),
               "`status` did not request exactly one report:\n%s", result.output.c_str());
  RuntimeSettings afterRestart(Settings::load(storage));
  EMCOOL_CHECK(afterRestart.statusReport() && afterRestart.statusIntervalSeconds() == 60,
               "the status settings were not saved");
  result = run(storage, live, {"unset", "STATUS_INTERVAL_S"});
  EMCOOL_CHECK(result.status == 0 && live.statusIntervalSeconds() == 10, "unset did not restore the 10 s default");

  // Wi-Fi: off by default, and on only with both a network and an admin password. The passwords are
  // never printed; WPA's 8-63 characters or nothing.
  {
    MemoryStorage wifiStorage;
    RuntimeSettings fresh(Settings::load(wifiStorage));
    EMCOOL_CHECK(!fresh.boot().wifiEnabled(), "Wi-Fi is on by default");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", "garage"}).status == 0, "set WIFI_SSID failed");
    EMCOOL_CHECK(!Settings::load(wifiStorage).wifiEnabled(), "Wi-Fi came on with no admin password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "too short"}).status == 1,
                 "an 9-character admin password was accepted");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "a long admin phrase"}).status == 0,
                 "set ADMIN_PASSWORD failed");
    const Settings withPassword = Settings::load(wifiStorage);
    EMCOOL_CHECK(withPassword.wifiEnabled() && withPassword.wifiSsid == "garage" && withPassword.wifiPassword.empty() &&
                     withPassword.adminPassword == "a long admin phrase",
                 "the Wi-Fi settings did not load");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", "1234567"}).status == 1, "a 7-character WPA password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", std::string(64, 'x').c_str()}).status == 1,
                 "a 64-character WPA password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", "wpa password"}).status == 0, "set WIFI_PASSWORD failed");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", ""}).status == 1, "an empty WIFI_SSID was accepted");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", std::string(33, 'n').c_str()}).status == 1,
                 "a 33-character WIFI_SSID was accepted");
    const Result shown = run(wifiStorage, fresh, {"show"});
    EMCOOL_CHECK(!contains(shown.output, "wpa password") && !contains(shown.output, "a long admin phrase") &&
                     contains(shown.output, "WIFI_PASSWORD      set (12 characters)") &&
                     contains(shown.output, "ADMIN_PASSWORD     set (19 characters)") &&
                     contains(shown.output, "WIFI_SSID          garage"),
                 "show printed a password, or not the Wi-Fi settings:\n%s", shown.output.c_str());
    EMCOOL_CHECK(run(wifiStorage, fresh, {"unset", "WIFI_SSID"}).status == 0 && !Settings::load(wifiStorage).wifiEnabled(),
                 "unset WIFI_SSID did not turn Wi-Fi off");
  }

  // The schedule: every interval while on, nothing while off, and a fresh window on resuming.
  {
    MemoryStorage scheduleStorage;
    RuntimeSettings running(Settings::load(scheduleStorage));
    StatusSchedule schedule;
    EMCOOL_CHECK(!schedule.decide(running, false, 0).report, "a report at the very first look");
    EMCOOL_CHECK(!schedule.decide(running, false, 9999).report, "a report before 10 s");
    auto decision = schedule.decide(running, false, 10000);
    EMCOOL_CHECK(decision.report && decision.restartWindow, "no report at 10 s");
    decision = schedule.decide(running, true, 12000);
    EMCOOL_CHECK(decision.report && !decision.restartWindow, "`status` did not report, or moved the window");
    EMCOOL_CHECK(schedule.decide(running, false, 20000).report, "an on-demand report shifted the cadence");
    run(scheduleStorage, running, {"status", "off"});
    decision = schedule.decide(running, false, 21000);
    EMCOOL_CHECK(!decision.report && decision.restartWindow, "turning off did not start a fresh window");
    EMCOOL_CHECK(!schedule.decide(running, false, 100000).report, "a periodic report while off");
    EMCOOL_CHECK(schedule.decide(running, true, 100001).report, "`status` while off did not report");
    run(scheduleStorage, running, {"status", "on"});
    decision = schedule.decide(running, false, 200000);
    EMCOOL_CHECK(!decision.report && decision.restartWindow, "resuming reported at once instead of starting a window");
    EMCOOL_CHECK(!schedule.decide(running, false, 209999).report, "a report before 10 s after resuming");
    EMCOOL_CHECK(schedule.decide(running, false, 210000).report, "no report 10 s after resuming");
  }

  // The Wi-Fi console: the password first, then the same commands as the USB console.
  {
    MemoryStorage remoteStorage;
    RuntimeSettings running(Settings::load(remoteStorage));
    std::vector<std::vector<std::string>> ran;
    RemoteConsole console("a long admin phrase", [&](int count, char** words, std::string& output) {
      ran.emplace_back(words, words + count);
      runSettingsCommand(remoteStorage, running, count, words, output);
    });
    const auto send = [&](std::string_view bytes, std::int64_t nowMs, bool& close) {
      return console.receive(std::span<const char>(bytes.data(), bytes.size()), nowMs, close);
    };
    bool close = false;
    std::string out = console.connected(0);
    EMCOOL_CHECK(contains(out, "password: ") && contains(out, "plain text"), "no password prompt:\n%s", out.c_str());
    out = send("show\r\n", 0, close);
    EMCOOL_CHECK(!console.authenticated() && !close && ran.empty() && contains(out, "wrong password"),
                 "a command ran before the password:\n%s", out.c_str());
    // A telnet client's negotiation, a CR NUL line end and a backspace don't get in the way.
    const std::string negotiated = std::string("\xff\xfb\x18\xff\xfa\x18", 6) + std::string(1, '\0') + "xterm" +
                                   "\xff\xf0" "a long admin phrasX\x7f" "e\r";
    out = send(negotiated, 0, close);
    EMCOOL_CHECK(console.authenticated() && !close && contains(out, "logged in") && contains(out, "emcool> "),
                 "the right password, through telnet negotiation, was refused:\n%s", out.c_str());
    out = send("\nset WIFI_SSID \"garage wifi\"\nshow\n", 0, close);
    const std::vector<std::string> expectedSet = {"set", "WIFI_SSID", "garage wifi"};
    EMCOOL_CHECK(ran.size() == 2 && ran[0] == expectedSet &&
                     remoteStorage.values["wifi_ssid"] == "garage wifi" && contains(out, "WIFI_SSID saved") &&
                     contains(out, "\r\n") && contains(out, "WIFI_SSID          garage wifi"),
                 "commands over the Wi-Fi console:\n%s", out.c_str());
    out = send(std::string(RemoteConsole::kMaxLineBytes + 10, 'x') + "\n", 0, close);
    EMCOOL_CHECK(ran.size() == 2 && contains(out, "longer than"), "an overlong line ran:\n%s", out.c_str());
    out = send("exit\n", 0, close);
    EMCOOL_CHECK(close && contains(out, "bye"), "exit did not close");

    // Three wrong passwords end the connection, and lock everyone out for a while after.
    console.connected(1000);
    send("one\n", 1000, close);
    send("two\n", 1000, close);
    EMCOOL_CHECK(!close, "closed before the third wrong password");
    out = send("three\n", 1000, close);
    EMCOOL_CHECK(close && !console.authenticated(), "the third wrong password did not close the connection");
    console.connected(2000);
    out = send("a long admin phrase\n", 2000, close);
    EMCOOL_CHECK(close && !console.authenticated() && contains(out, "too many"),
                 "the right password got in during the lockout:\n%s", out.c_str());
    console.connected(1000 + RemoteConsole::kLockoutMs);
    out = send("a long admin phrase\n", 1000 + RemoteConsole::kLockoutMs, close);
    EMCOOL_CHECK(!close && console.authenticated(), "the lockout did not end:\n%s", out.c_str());
    // A new connection starts logged out.
    console.connected(9000);
    EMCOOL_CHECK(!console.authenticated(), "a new connection inherited the last one's login");
    // HTTP Basic: base64("user:pass"), user ignored, padding of every length, nonsense refused.
    EMCOOL_CHECK(basicAuthPassword("Basic dXNlcjpwYXNz") == std::string("pass"), "Basic user:pass");
    EMCOOL_CHECK(basicAuthPassword("basic OmFiYw==") == std::string("abc"), "Basic :abc, lower-case scheme");
    EMCOOL_CHECK(basicAuthPassword("Basic eDphYg==") == std::string("ab"), "Basic x:ab");
    EMCOOL_CHECK(basicAuthPassword("Basic eDph") == std::string("a"), "Basic x:a");
    EMCOOL_CHECK(basicAuthPassword("Basic eDo=") == std::string(""), "Basic x:");
    EMCOOL_CHECK(basicAuthPassword("Basic YSBsb25nOmEgbG9uZyBhZG1pbiBwaHJhc2U=") == std::string("a long admin phrase"),
                 "Basic with spaces in the password");
    for (const char* bad : {"Bearer dXNlcjpwYXNz", "Basic", "Basic ", "Basic dXNlcjpwYXN", "Basic dXNlcnBhc3M=",
                            "Basic dX=lcjpwYXNz", "Basic dXNl*jpwYXNz"}) {
      EMCOOL_CHECK(!basicAuthPassword(bad), "accepted %s", bad);
    }
    EMCOOL_CHECK(withCrLf("a\nb\n") == "a\r\nb\r\n", "withCrLf");
    EMCOOL_CHECK(!constantTimeEquals("a long admin phrase", "a long admin phras") &&
                     !constantTimeEquals("", "x") && constantTimeEquals("same", "same"),
                 "constantTimeEquals is wrong");
  }

  std::printf("check_settings: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
