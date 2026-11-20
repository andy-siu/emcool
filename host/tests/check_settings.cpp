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
#include "emcool/settings/line_editor.hpp"
#include "emcool/settings/login_limiter.hpp"
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
  EMCOOL_CHECK(run(storage, rebooted, {"set", "WIFI_MODE", "1"}).status == 1, "an unknown setting was accepted");
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
    EMCOOL_CHECK(!running.boot().obdEnabled && !running.boot().serviceModeEnabled,
                 "a transmitting feature is on by default");
    Result shown = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(shown.output, "OBD_ENABLED        0 (default)") &&
                     contains(shown.output, "SERVICE_MODE_ENABLED 0 (default)"),
                 "show on a fresh store:\n%s", shown.output.c_str());
    shown = run(fresh, running, {"set", "obd_enabled", "on"});
    EMCOOL_CHECK(shown.status == 0 && fresh.values["obd_enabled"] == "1" && contains(shown.output, "after `restart`") &&
                     !running.boot().obdEnabled,
                 "set OBD_ENABLED on:\n%s", shown.output.c_str());
    shown = run(fresh, running, {"show"});
    EMCOOL_CHECK(contains(shown.output, "OBD_ENABLED        1   <- takes effect after `restart` (running: 0)"),
                 "show with OBD waiting for a restart:\n%s", shown.output.c_str());
    const Settings restarted = Settings::load(fresh);
    EMCOOL_CHECK(restarted.obdEnabled, "OBD_ENABLED did not load");
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

  // Wi-Fi: off by default, and on only with a network and a password for something it serves. The
  // passwords are never printed; WPA's 8-63 characters or nothing.
  {
    MemoryStorage wifiStorage;
    RuntimeSettings fresh(Settings::load(wifiStorage));
    EMCOOL_CHECK(!fresh.boot().wifiEnabled(), "Wi-Fi is on by default");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", "garage"}).status == 0, "set WIFI_SSID failed");
    EMCOOL_CHECK(!Settings::load(wifiStorage).wifiEnabled(), "Wi-Fi came on with no password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "1234567"}).status == 1,
                 "a 7-character admin password was accepted");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "12345678"}).status == 0,
                 "an 8-character admin password was refused");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "a long admin phrase"}).status == 0,
                 "set ADMIN_PASSWORD failed");
    const Settings withAdmin = Settings::load(wifiStorage);
    EMCOOL_CHECK(withAdmin.wifiEnabled() && withAdmin.sshEnabled() && withAdmin.wifiSsid == "garage" &&
                     withAdmin.wifiPassword.empty() && withAdmin.adminPassword == "a long admin phrase" &&
                     withAdmin.telnetPassword.empty() && !withAdmin.telnetEnabled(),
                 "the Wi-Fi settings did not load");
    // The SSH password lives under a key of its own; the old ADMIN_PASSWORD key is the telnet one's.
    EMCOOL_CHECK(wifiStorage.values["ssh_admin_pw"] == "a long admin phrase" && !wifiStorage.values.contains("admin_password"),
                 "ADMIN_PASSWORD was not saved under ssh_admin_pw");
    // The two passwords must differ: the telnet one crosses the network in plain text.
    const Result same = run(wifiStorage, fresh, {"set", "TELNET_PASSWORD", "a long admin phrase"});
    EMCOOL_CHECK(same.status == 1 && contains(same.output, "must differ") && !wifiStorage.values.contains("admin_password"),
                 "TELNET_PASSWORD was allowed to equal ADMIN_PASSWORD:\n%s", same.output.c_str());
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "TELNET_PASSWORD", "a telnet phrase"}).status == 0,
                 "set TELNET_PASSWORD failed");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "ADMIN_PASSWORD", "a telnet phrase"}).status == 1,
                 "ADMIN_PASSWORD was allowed to equal TELNET_PASSWORD");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", "1234567"}).status == 1, "a 7-character WPA password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", std::string(64, 'x').c_str()}).status == 1,
                 "a 64-character WPA password");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_PASSWORD", "wpa password"}).status == 0, "set WIFI_PASSWORD failed");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", ""}).status == 1, "an empty WIFI_SSID was accepted");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "WIFI_SSID", std::string(33, 'n').c_str()}).status == 1,
                 "a 33-character WIFI_SSID was accepted");
    const Result shown = run(wifiStorage, fresh, {"show"});
    EMCOOL_CHECK(!contains(shown.output, "wpa password") && !contains(shown.output, "a long admin phrase") &&
                     !contains(shown.output, "a telnet phrase") &&
                     contains(shown.output, "WIFI_PASSWORD      set (12 characters)") &&
                     contains(shown.output, "ADMIN_PASSWORD     set (19 characters)") &&
                     contains(shown.output, "TELNET_PASSWORD    set (15 characters)") &&
                     contains(shown.output, "WIFI_SSID          garage"),
                 "show printed a password, or not the Wi-Fi settings:\n%s", shown.output.c_str());
    // The telnet console: off by default, on with TELNET_CONSOLE 1 and a TELNET_PASSWORD, and only at the restart.
    EMCOOL_CHECK(!Settings::load(wifiStorage).telnetConsole && !Settings::load(wifiStorage).telnetEnabled(),
                 "TELNET_CONSOLE is on by default");
    const Result consoleOn = run(wifiStorage, fresh, {"set", "TELNET_CONSOLE", "on"});
    EMCOOL_CHECK(consoleOn.status == 0 && wifiStorage.values["telnet_console"] == "1" &&
                     Settings::load(wifiStorage).telnetEnabled() && contains(consoleOn.output, "after `restart`"),
                 "set TELNET_CONSOLE on:\n%s", consoleOn.output.c_str());
    EMCOOL_CHECK(run(wifiStorage, fresh, {"unset", "TELNET_PASSWORD"}).status == 0 &&
                     !Settings::load(wifiStorage).telnetEnabled() && Settings::load(wifiStorage).wifiEnabled(),
                 "the telnet console stayed on without a password, or Wi-Fi went off with SSH still set");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"unset", "TELNET_CONSOLE"}).status == 0 && !Settings::load(wifiStorage).telnetConsole,
                 "unset TELNET_CONSOLE did not turn the console back off");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"unset", "ADMIN_PASSWORD"}).status == 0 && !Settings::load(wifiStorage).wifiEnabled(),
                 "Wi-Fi stayed on with neither password");
    // Telnet's password alone turns Wi-Fi on, with SSH off.
    EMCOOL_CHECK(run(wifiStorage, fresh, {"set", "TELNET_PASSWORD", "a telnet phrase"}).status == 0 &&
                     Settings::load(wifiStorage).wifiEnabled() && !Settings::load(wifiStorage).sshEnabled(),
                 "TELNET_PASSWORD alone did not turn Wi-Fi on with SSH off");
    EMCOOL_CHECK(run(wifiStorage, fresh, {"unset", "WIFI_SSID"}).status == 0 && !Settings::load(wifiStorage).wifiEnabled(),
                 "unset WIFI_SSID did not turn Wi-Fi off");
  }

  // WIFI_ENABLED: on by default, live, and it turns Wi-Fi off and on without touching the network or
  // the passwords. Only the switch: the network still waits for the restart.
  {
    MemoryStorage wifiStorage;
    wifiStorage.values["wifi_ssid"] = "garage";
    wifiStorage.values["ssh_admin_pw"] = "a long admin phrase";
    RuntimeSettings running(Settings::load(wifiStorage));
    EMCOOL_CHECK(running.boot().wifiSwitchedOn && running.boot().wifiEnabled() && running.wifiSwitchedOn(),
                 "WIFI_ENABLED is off by default");
    // Before the firmware hands over its switch, it saves and waits for the restart like the rest.
    Result early = run(wifiStorage, running, {"set", "WIFI_ENABLED", "0"});
    EMCOOL_CHECK(early.status == 0 && contains(early.output, "after `restart`") && running.wifiSwitchedOn() &&
                     wifiStorage.values["wifi_enabled"] == "0" && !Settings::load(wifiStorage).wifiEnabled() &&
                     Settings::load(wifiStorage).wifiSetUp(),
                 "set WIFI_ENABLED 0 with no switch:\n%s", early.output.c_str());
    EMCOOL_CHECK(run(wifiStorage, running, {"unset", "WIFI_ENABLED"}).status == 0, "unset WIFI_ENABLED failed");

    std::vector<bool> switched;
    bool works = true;
    running.setWifiSwitch([&](bool on) {
      switched.push_back(on);
      return works;
    });
    Result off = run(wifiStorage, running, {"set", "WIFI_ENABLED", "off"});
    EMCOOL_CHECK(off.status == 0 && switched == std::vector<bool>({false}) && !running.wifiSwitchedOn() &&
                     contains(off.output, "Wi-Fi is off now") && wifiStorage.values["wifi_enabled"] == "0" &&
                     wifiStorage.values["wifi_ssid"] == "garage" && wifiStorage.values["ssh_admin_pw"] == "a long admin phrase",
                 "set WIFI_ENABLED off did not switch Wi-Fi off, or touched the network:\n%s", off.output.c_str());
    Result shown = run(wifiStorage, running, {"show"});
    EMCOOL_CHECK(contains(shown.output, "WIFI_ENABLED       0") && !contains(shown.output, "WIFI_ENABLED       0   <-"),
                 "show waits for a restart that WIFI_ENABLED doesn't need:\n%s", shown.output.c_str());
    Result again = run(wifiStorage, running, {"set", "WIFI_ENABLED", "0"});
    EMCOOL_CHECK(again.status == 0 && switched.size() == 1 && contains(again.output, "already off"),
                 "switching Wi-Fi off twice:\n%s", again.output.c_str());
    Result on = run(wifiStorage, running, {"unset", "WIFI_ENABLED"});
    EMCOOL_CHECK(on.status == 0 && switched == std::vector<bool>({false, true}) && running.wifiSwitchedOn() &&
                     contains(on.output, "Wi-Fi is on now, joining \"garage\"") && !wifiStorage.values.contains("wifi_enabled"),
                 "unset WIFI_ENABLED did not switch Wi-Fi back on:\n%s", on.output.c_str());
    // A switch that fails leaves it where it was, saved or not, and says so.
    works = false;
    Result failed = run(wifiStorage, running, {"set", "WIFI_ENABLED", "0"});
    EMCOOL_CHECK(failed.status == 0 && running.wifiSwitchedOn() && contains(failed.output, "did not stop"),
                 "a failed switch:\n%s", failed.output.c_str());
    // With nothing set up this boot, the switch only moves; there is nothing to start.
    MemoryStorage bare;
    RuntimeSettings unset(Settings::load(bare));
    std::vector<bool> bareSwitched;
    unset.setWifiSwitch([&](bool value) {
      bareSwitched.push_back(value);
      return true;
    });
    Result bareOff = run(bare, unset, {"set", "WIFI_ENABLED", "0"});
    Result bareOn = run(bare, unset, {"set", "WIFI_ENABLED", "1"});
    EMCOOL_CHECK(bareSwitched.empty() && unset.wifiSwitchedOn() && contains(bareOff.output, "Wi-Fi is off now") &&
                     contains(bareOn.output, "no WIFI_SSID"),
                 "the switch on a bike with no network:\n%s\n%s", bareOff.output.c_str(), bareOn.output.c_str());
  }

  // A bike that had ADMIN_PASSWORD before SSH: its password is the telnet one now, and SSH stays off.
  {
    MemoryStorage upgraded;
    upgraded.values["wifi_ssid"] = "garage";
    upgraded.values["admin_password"] = "an old admin phrase";
    const Settings loaded = Settings::load(upgraded);
    EMCOOL_CHECK(loaded.telnetPassword == "an old admin phrase" && loaded.adminPassword.empty() && !loaded.sshEnabled() &&
                     loaded.wifiEnabled() && !loaded.telnetEnabled(),
                 "the old ADMIN_PASSWORD did not become TELNET_PASSWORD, or SSH came on");
  }

  // The telnet console runs only what looks.
  {
    const auto allows = [](std::vector<const char*> words) {
      return readOnlyAllows(static_cast<int>(words.size()), words.data());
    };
    for (const auto& words : std::vector<std::vector<const char*>>{
             {"help"}, {"show"}, {"status"}, {"ifconfig"}, {"update", "status"}, {"dir"}, {"ls", "/RIDES"},
             {"cd", "RIDES"}, {"pwd"}, {"type", "/a.txt"}, {"cat", "/a.txt"}}) {
      EMCOOL_CHECK(allows(words), "the telnet console refused %s", words[0]);
    }
    for (const auto& words : std::vector<std::vector<const char*>>{
             {"set", "ADMIN_PASSWORD", "x"}, {"set", "TELNET_CONSOLE", "0"}, {"unset", "WIFI_SSID"}, {"status", "off"},
             {"status", "every", "60"}, {"restart"}, {"update", "/emcool.bin"}, {"rm", "/a.txt"}, {"cp", "a", "b"},
             {"mv", "a", "b"}, {"mkdir", "x"}, {"rmdir", "x"}, {"psu", "on"}, {"psu"}, {"poller", "off"},
             {"can-send", "7DF", "02"}, {"can-repeat", "150", "500"}, {"can-dump"}, {}}) {
      EMCOOL_CHECK(!allows(words), "the telnet console allowed %s", words.empty() ? "(nothing)" : words[0]);
    }
    EMCOOL_CHECK(isFullConsoleCommand("psu") && isFullConsoleCommand("set") && !isFullConsoleCommand("rm-rf"),
                 "isFullConsoleCommand");
  }

  // SSH's login: three tries a connection, and a lockout for everyone after three wrong in a row.
  {
    LoginLimiter unset;
    unset.connected();
    EMCOOL_CHECK(unset.check("", "", 0) != LoginLimiter::Verdict::kAccepted, "an empty password logged in");
    LoginLimiter limiter;
    limiter.connected();
    EMCOOL_CHECK(limiter.check("a long admin phrase", "a long admin phrase", 0) == LoginLimiter::Verdict::kAccepted,
                 "the right password was refused");
    limiter.connected();
    EMCOOL_CHECK(limiter.check("one", "pw", 1000) == LoginLimiter::Verdict::kWrong, "first wrong");
    EMCOOL_CHECK(limiter.check("two", "pw", 1000) == LoginLimiter::Verdict::kWrong, "second wrong");
    EMCOOL_CHECK(limiter.check("three", "pw", 1000) == LoginLimiter::Verdict::kWrongLast, "third wrong didn't end it");
    limiter.connected();
    EMCOOL_CHECK(limiter.check("pw", "pw", 2000) == LoginLimiter::Verdict::kLockedOut,
                 "the right password got in during the lockout");
    limiter.connected();
    EMCOOL_CHECK(limiter.check("pw", "pw", 1000 + LoginLimiter::kLockoutMs) == LoginLimiter::Verdict::kAccepted,
                 "the lockout did not end");
  }

  // The SSH console's line editing.
  {
    LineEditor terminal(true);
    LineEditor::Result result = terminal.feed("sho");
    EMCOOL_CHECK(result.echo == "sho" && result.lines.empty(), "typing didn't echo");
    result = terminal.feed("x\x7fw\r");
    EMCOOL_CHECK(result.echo == "x\b \bw\r\n" && result.lines == std::vector<std::string>{"show"},
                 "backspace and Return: [%s]", result.echo.c_str());
    result = terminal.feed("\x1b" "[Aset" "\x1b" "[D x\r\n");
    EMCOOL_CHECK(result.lines == std::vector<std::string>{"set x"}, "arrow keys weren't swallowed");
    result = terminal.feed("rm -r /\x15" "dir\r");
    EMCOOL_CHECK(result.lines == std::vector<std::string>{"dir"}, "Ctrl-U didn't clear the line");
    result = terminal.feed("restart\x03");
    EMCOOL_CHECK(result.interrupted && result.lines.empty() && terminal.pending().empty(), "Ctrl-C didn't drop the line");
    result = terminal.feed("\x04");
    EMCOOL_CHECK(result.endOfInput, "Ctrl-D on an empty line didn't end");
    terminal.hideNextLine();
    result = terminal.feed("secret\r");
    EMCOOL_CHECK(result.echo == "\r\n" && result.lines == std::vector<std::string>{"secret"}, "a hidden line echoed");
    result = terminal.feed(std::string(LineEditor::kMaxLineBytes + 5, 'x') + "\r");
    EMCOOL_CHECK(result.lines.size() == 1 && result.lines[0].empty() && contains(result.echo, "longer than"),
                 "an overlong line wasn't refused");
    LineEditor plain(false);
    result = plain.feed("show\nstatus\n");
    EMCOOL_CHECK(result.echo.empty() && result.lines == (std::vector<std::string>{"show", "status"}),
                 "without a terminal: echoed, or lines wrong");
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

  // The telnet console: the password first, then commands.
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
    // Every request is answered, or a telnet client stays in character mode and shows Return as ^M:
    // DO SGA refused (WONT), WILL TTYPE refused (DONT), DO ECHO agreed to silently while the password is
    // typed, and the client's DONT ECHO after the login's WONT ECHO not answered again.
    EMCOOL_CHECK(contains(out, std::string("\xff\xfe\x18", 3)), "WILL TTYPE got no DONT:\n%s", out.c_str());
    {
      RemoteConsole fresh("pw", [](int, char**, std::string&) {});
      fresh.connected(0);
      out = fresh.receive(std::span<const char>("\xff\xfd\x03\xff\xfd\x01", 6), 0, close);
      EMCOOL_CHECK(out == std::string("\xff\xfc\x03", 3), "DO SGA, DO ECHO answered wrong");
      out = fresh.receive(std::span<const char>("pw\r", 3), 0, close);
      EMCOOL_CHECK(fresh.authenticated() && contains(out, std::string("\xff\xfc\x01", 3)), "no WONT ECHO at login");
      out = fresh.receive(std::span<const char>("\xff\xfe\x01", 3), 0, close);
      EMCOOL_CHECK(out.empty(), "DONT ECHO after WONT ECHO was answered again");
    }
    out = send("\nset WIFI_SSID \"garage wifi\"\nshow\n", 0, close);
    const std::vector<std::string> expectedSet = {"set", "WIFI_SSID", "garage wifi"};
    EMCOOL_CHECK(ran.size() == 2 && ran[0] == expectedSet &&
                     remoteStorage.values["wifi_ssid"] == "garage wifi" && contains(out, "WIFI_SSID saved") &&
                     contains(out, "\r\n") && contains(out, "WIFI_SSID          garage wifi"),
                 "commands through the console:\n%s", out.c_str());
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
