// The settings and the serial console's commands, against an in-memory store in place of NVS.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/log.hpp"
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
  result.status = runSettingsCommand(storage, running, static_cast<int>(arguments.size()), arguments.data(),
                                     result.output);
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

  // The secret: validated, stored exactly, never echoed.
  result = run(storage, booted, {"set", "BLE_SECRET", "too short"});
  EMCOOL_CHECK(result.status == 1 && contains(result.output, "12-128 characters"), "a short secret: %s",
               result.output.c_str());
  const char* secret = "correct horse  battery staple";
  result = run(storage, booted, {"set", "BLE_SECRET", secret});
  EMCOOL_CHECK(result.status == 0 && storage.values["ble_secret"] == secret, "the secret was not stored exactly");
  result = run(storage, booted, {"show"});
  EMCOOL_CHECK(!contains(result.output, "horse") && contains(result.output, "set (29 characters)") &&
                   contains(result.output, "takes effect after `restart`"),
               "show after setting the secret:\n%s", result.output.c_str());
  result = run(storage, booted, {"set", "BLE_SECRET", "a", "b"});
  EMCOOL_CHECK(result.status == 1 && contains(result.output, "quote"), "an unquoted phrase was accepted");

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

  std::printf("check_settings: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
