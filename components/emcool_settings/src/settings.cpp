#include "emcool/settings/settings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

#include "emcool/log.hpp"

namespace emcool::settings {

namespace {

constexpr const char* kTag = "settings";

constexpr SettingSpec kSpecs[] = {
    {"BLE_SECRET", "ble_secret", SettingType::kSecret, "", 8, 64, false,
     "The phone dashboard's Bluetooth key: 8-64 characters with at least one letter and one digit (no spaces, "
     "quotes or backslashes), or 64 hex characters, such as `openssl rand -hex 32` or the dashboard's New key makes. "
     "A short key can be guessed from one overheard connection, so make it long. Setting it turns Bluetooth on at "
     "once; unset, Bluetooth stays off."},
    // The scan response has room for a 29-byte name.
    {"BLE_NAME", "ble_name", SettingType::kText, "emcool", 1, 29, false,
     "The name the phone's device list shows. The dashboard finds the bike by its service UUID, not this."},
    {"CUSTOM_BMS_CONFIG", "custom_bms", SettingType::kFlag, "0", 0, 0, false,
     "1 only if the BMS runs the custom LiBAL config. The frames still win."},
    // Everything that puts frames on the bike's bus is off until asked for, OBD_ENABLED and
    // ELOCK_ENABLED included.
    {"OBD_ENABLED", "obd_enabled", SettingType::kFlag, "0", 0, 0, false,
     "1 to poll OBD-II (speed, rpm, temperatures, 12 V, trouble codes). Off, "
     "emcool only listens: the CAN controller doesn't even acknowledge frames."},
    {"ELOCK_ENABLED", "elock_enabled", SettingType::kFlag, "0", 0, 0, false,
     "1 to read keys_paired from the E-LOCK once at startup. Needs OBD_ENABLED."},
    {"SERVICE_MODE_ENABLED", "service_mode", SettingType::kFlag, "0", 0, 0, false,
     "1 to let the dashboard read the VCU's calibration parameters (Menu → Service mode), "
     "only while the bike is parked and out of drive. Needs OBD_ENABLED."},
    {"SERVICE_WRITE_ENABLED", "service_write", SettingType::kFlag, "0", 0, 0, false,
     "1 to let the dashboard WRITE to the VCU (/vcu-write: allowlisted parameters, set service point, sync the "
     "clock, clear DTCs, charge current/stop, SOC limit, reset). Off by default "
     "and the only transmitting switch so on purpose: a web page can change the bike. Needs OBD_ENABLED and "
     "SERVICE_MODE_ENABLED."},
    {"CHARGE_AUTO_ENABLED", "charge_auto", SettingType::kFlag, "0", 0, 0, false,
     "1 to let the automatic charge-current controller command the bike's charge current on a timer. "
     "Needs SERVICE_WRITE_ENABLED — it commands writes. The most autonomous feature here."},
    // Wi-Fi: off until WIFI_SSID and ADMIN_PASSWORD are both set. The bike joins that network and serves the
    // console on port 23 and the ride log on port 80, both behind ADMIN_PASSWORD.
    {"WIFI_SSID", "wifi_ssid", SettingType::kText, "", 0, 32, false,
     "The Wi-Fi network to join (garage Wi-Fi or a phone hotspot), 1-32 characters; unset keeps Wi-Fi off. The bike "
     "then serves this console on port 23 (`nc <bike> 23`) and the ride log on http://<bike>/, both behind "
     "ADMIN_PASSWORD."},
    {"WIFI_PASSWORD", "wifi_password", SettingType::kSecret, "", 0, 63, false,
     "That network's password: 8-63 characters for WPA2/WPA3, or unset for an open network."},
    {"ADMIN_PASSWORD", "admin_password", SettingType::kSecret, "", 12, 128, false,
     "The password the console on port 23 and the ride-log page ask for, 12-128 characters. Unset keeps Wi-Fi off. "
     "It crosses the network in plain text, so use one used nowhere else."},
    {"STATUS_REPORT", "status_report", SettingType::kFlag, "1", 0, 0, true,
     "Whether the serial console prints the periodic status report. `status on|off` is the shorthand."},
    {"STATUS_INTERVAL_S", "status_every_s", SettingType::kInteger, "10", 5, 3600, true,
     "Seconds between status reports, 5-3600. `status every SECONDS` is the shorthand."},
};

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
  return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
         });
}

std::string lowerCased(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return lowered;
}

// What the dashboard's New key makes: 32 random bytes as 64 lower-case hex.
bool isGeneratedKey(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

// A generated key, or a typed one: 8-64 printable ASCII characters with at least one letter and one
// digit, and no spaces, quotes or backslashes, which the consoles would split or unescape. Must
// match BleSession::secretIsUsable() and the dashboard's validBleSecret() (lib/ble-secret.js).
bool isBluetoothKey(std::string_view value) {
  if (isGeneratedKey(value)) return true;
  const auto has = [&](auto test) { return std::any_of(value.begin(), value.end(), test); };
  return value.size() >= 8 && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char character) {
           return character > ' ' && character <= '~' && character != '"' && character != '\\';
         }) &&
         has([](char character) {
           return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
         }) &&
         has([](char character) { return character >= '0' && character <= '9'; });
}

std::optional<bool> parseFlag(std::string_view value) {
  for (const char* yes : {"1", "on", "true", "yes"}) {
    if (equalsIgnoringCase(value, yes)) return true;
  }
  for (const char* no : {"0", "off", "false", "no"}) {
    if (equalsIgnoringCase(value, no)) return false;
  }
  return std::nullopt;
}

std::optional<std::int64_t> parseInteger(std::string_view value) {
  std::int64_t number = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size()) return std::nullopt;
  return number;
}

std::string storedOrDefault(SettingsStorage& storage, const SettingSpec& spec) {
  const std::optional<std::string> stored = storage.read(spec.storageKey);
  if (!stored) return std::string(spec.defaultValue);
  if (const std::optional<std::string> problem = validate(spec, *stored)) {
    logWarning(kTag, "ignoring the stored %.*s (%s); using its default", static_cast<int>(spec.name.size()),
               spec.name.data(), problem->c_str());
    return std::string(spec.defaultValue);
  }
  return normalizeValue(spec, *stored);
}

std::string describe(const SettingSpec& spec, std::string_view value) { return describeValue(spec, value); }

void appendLine(std::string& output, const std::string& line) {
  output += line;
  output += '\n';
}

std::string whenItApplies(const SettingSpec& spec) {
  return spec.live ? "in effect now" : "it takes effect after `restart`";
}

int show(SettingsStorage& storage, const RuntimeSettings& running, std::string& output) {
  for (const SettingSpec& spec : kSpecs) {
    const std::optional<std::string> stored = storage.read(spec.storageKey);
    const std::string next = storedOrDefault(storage, spec);
    const std::string now = running.valueOf(spec);
    char line[160];
    std::snprintf(line, sizeof line, "%-18.*s %s%s", static_cast<int>(spec.name.size()), spec.name.data(),
                  describe(spec, next).c_str(), stored ? "" : " (default)");
    std::string text = line;
    if (next != now) {
      text += "   <- takes effect after `restart` (running: " + describe(spec, now) + ")";
    }
    appendLine(output, text);
  }
  return 0;
}

// Validates, saves and, for a live setting, applies. `name` is the user's spelling.
int save(SettingsStorage& storage, RuntimeSettings& running, const SettingSpec& spec, std::string_view value,
         std::string& output) {
  if (const std::optional<std::string> problem = validate(spec, value)) {
    appendLine(output, std::string(spec.name) + ": " + *problem);
    return 1;
  }
  const std::string normalized = normalizeValue(spec, value);
  if (!storage.write(spec.storageKey, normalized)) {
    appendLine(output, std::string(spec.name) + ": could not save it to NVS");
    return 1;
  }
  running.apply(spec, normalized);
  running.noteChange();
  if (spec.name == "BLE_SECRET") {
    switch (running.startBluetooth(normalized)) {
      case RuntimeSettings::BluetoothStart::kStarted:
        appendLine(output, "BLE_SECRET saved; Bluetooth is on now. Enter the same key in the dashboard and tap Connect");
        return 0;
      case RuntimeSettings::BluetoothStart::kFailed:
        appendLine(output, "BLE_SECRET saved, but Bluetooth did not start (the log above says why); `restart` tries "
                           "again");
        return 0;
      case RuntimeSettings::BluetoothStart::kAlreadyOn:
      case RuntimeSettings::BluetoothStart::kNoStarter:
        break;
    }
  }
  appendLine(output, std::string(spec.name) + " saved; " + whenItApplies(spec));
  return 0;
}

int set(SettingsStorage& storage, RuntimeSettings& running, int argumentCount, const char* const* arguments,
        std::string& output) {
  if (argumentCount != 3) {
    appendLine(output, "usage: set NAME VALUE — quote a value with spaces: set WIFI_SSID \"garage wifi\"");
    return 1;
  }
  const SettingSpec* spec = findSetting(arguments[1]);
  if (spec == nullptr) {
    appendLine(output, std::string("no setting called ") + arguments[1] + "; `show` lists them");
    return 1;
  }
  return save(storage, running, *spec, arguments[2], output);
}

int unset(SettingsStorage& storage, RuntimeSettings& running, int argumentCount, const char* const* arguments,
          std::string& output) {
  if (argumentCount != 2) {
    appendLine(output, "usage: unset NAME");
    return 1;
  }
  const SettingSpec* spec = findSetting(arguments[1]);
  if (spec == nullptr) {
    appendLine(output, std::string("no setting called ") + arguments[1] + "; `show` lists them");
    return 1;
  }
  if (!storage.erase(spec->storageKey)) {
    appendLine(output, std::string(spec->name) + ": could not remove it from NVS");
    return 1;
  }
  running.apply(*spec, spec->defaultValue);
  running.noteChange();
  appendLine(output, std::string(spec->name) + " is back to its default (" + describe(*spec, spec->defaultValue) +
                         "); " + whenItApplies(*spec));
  return 0;
}

std::string describeStatusReport(const RuntimeSettings& running) {
  if (!running.statusReport()) return "status report: off (`status on` to resume)";
  return "status report: every " + std::to_string(running.statusIntervalSeconds()) + " s";
}

// Shorthands for STATUS_REPORT and STATUS_INTERVAL_S, saved like any `set`.
int status(SettingsStorage& storage, RuntimeSettings& running, int argumentCount, const char* const* arguments,
           std::string& output) {
  const std::string_view mode = argumentCount >= 2 ? arguments[1] : "";
  int result = 0;
  if (argumentCount == 1) {
    running.requestReport();
  } else if (argumentCount == 2 && (mode == "on" || mode == "off")) {
    result = save(storage, running, *findSetting("STATUS_REPORT"), mode, output);
  } else if (argumentCount == 3 && mode == "every") {
    result = save(storage, running, *findSetting("STATUS_INTERVAL_S"), arguments[2], output);
    if (result == 0 && !running.statusReport()) {
      result = save(storage, running, *findSetting("STATUS_REPORT"), "on", output);
    }
  } else {
    appendLine(output, "usage: status [on | off | every SECONDS]");
    return 1;
  }
  appendLine(output, describeStatusReport(running));
  return result;
}

}  // namespace

std::span<const SettingSpec> settingSpecs() { return kSpecs; }

const SettingSpec* findSetting(std::string_view name) {
  for (const SettingSpec& spec : kSpecs) {
    if (equalsIgnoringCase(spec.name, name)) return &spec;
  }
  return nullptr;
}

std::optional<std::string> validate(const SettingSpec& spec, std::string_view value) {
  if (spec.type == SettingType::kFlag) {
    if (!parseFlag(value)) return std::string("must be 1 or 0 (on/off, true/false and yes/no also work)");
    return std::nullopt;
  }
  if (spec.type == SettingType::kInteger) {
    const std::optional<std::int64_t> number = parseInteger(value);
    if (!number || *number < spec.minimum || *number > spec.maximum) {
      return "must be a whole number from " + std::to_string(spec.minimum) + " to " + std::to_string(spec.maximum);
    }
    return std::nullopt;
  }
  const auto length = static_cast<std::int64_t>(value.size());
  // Protocol 2 has no lockout, so a typed key must at least mix letters and digits.
  if (spec.name == "BLE_SECRET" && !isBluetoothKey(normalizeValue(spec, value))) {
    return std::string("must be 8-64 characters with at least one letter and one digit, and no spaces, quotes or "
                       "backslashes; or 64 hex characters (openssl rand -hex 32)");
  }
  // A Wi-Fi password is WPA's 8-63 characters, or nothing for an open network.
  if (spec.name == "WIFI_PASSWORD" && length > 0 && length < 8) {
    return "must be 8-63 characters (WPA2/WPA3), or unset for an open network; this is " + std::to_string(length);
  }
  if (spec.name == "WIFI_SSID" && length == 0) return std::string("must be 1-32 characters; `unset WIFI_SSID` turns Wi-Fi off");
  if (length < spec.minimum || length > spec.maximum) {
    return "must be " + std::to_string(spec.minimum) + "-" + std::to_string(spec.maximum) + " characters; this is " +
           std::to_string(length);
  }
  for (const char character : value) {
    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) {
      return std::string("can't hold control characters");
    }
  }
  return std::nullopt;
}

// The secret itself is never printed: only whether it's set, and its length.
std::string describeValue(const SettingSpec& spec, std::string_view value) {
  if (spec.type != SettingType::kSecret) return value.empty() ? "(empty)" : std::string(value);
  if (value.empty()) return "not set";
  // A typed key's length would narrow a guess at it, so the key shows no length.
  if (spec.name == "BLE_SECRET") return "set";
  return "set (" + std::to_string(value.size()) + " characters)";
}

std::string normalizeValue(const SettingSpec& spec, std::string_view value) {
  if (spec.type == SettingType::kFlag) return *parseFlag(value) ? "1" : "0";
  if (spec.type == SettingType::kInteger) return std::to_string(*parseInteger(value));
  // A generated key pasted in upper case is the same key; a typed key keeps its case.
  if (spec.name == "BLE_SECRET") {
    std::string lowered = lowerCased(value);
    return isGeneratedKey(lowered) ? lowered : std::string(value);
  }
  return std::string(value);
}

std::string savedValue(SettingsStorage& storage, const SettingSpec& spec, bool& stored) {
  const std::optional<std::string> value = storage.read(spec.storageKey);
  stored = value.has_value();
  if (!value || validate(spec, *value)) return std::string(spec.defaultValue);
  return normalizeValue(spec, *value);
}

Settings Settings::load(SettingsStorage& storage) {
  const auto value = [&](std::string_view name) { return storedOrDefault(storage, *findSetting(name)); };
  Settings settings;
  settings.bleSecret = value("BLE_SECRET");
  settings.bleName = value("BLE_NAME");
  settings.customBmsConfig = value("CUSTOM_BMS_CONFIG") == "1";
  settings.obdEnabled = value("OBD_ENABLED") == "1";
  settings.elockEnabled = value("ELOCK_ENABLED") == "1";
  settings.serviceModeEnabled = value("SERVICE_MODE_ENABLED") == "1";
  settings.serviceWriteEnabled = value("SERVICE_WRITE_ENABLED") == "1";
  settings.chargeAutoEnabled = value("CHARGE_AUTO_ENABLED") == "1";
  settings.wifiSsid = value("WIFI_SSID");
  settings.wifiPassword = value("WIFI_PASSWORD");
  settings.adminPassword = value("ADMIN_PASSWORD");
  settings.statusReport = value("STATUS_REPORT") == "1";
  settings.statusIntervalSeconds = static_cast<std::uint32_t>(*parseInteger(value("STATUS_INTERVAL_S")));
  return settings;
}

RuntimeSettings::RuntimeSettings(Settings boot)
    : boot_(std::move(boot)),
      statusReport_(boot_.statusReport),
      statusIntervalSeconds_(boot_.statusIntervalSeconds),
      bluetoothKey_(boot_.bleSecret) {}

void RuntimeSettings::setBluetoothStarter(BluetoothStarter starter, bool alreadyOn) {
  const std::lock_guard<std::mutex> lock(bluetoothMutex_);
  bluetoothStarter_ = std::move(starter);
  bluetoothOn_.store(alreadyOn, std::memory_order_relaxed);
}

RuntimeSettings::BluetoothStart RuntimeSettings::startBluetooth(const std::string& key) {
  BluetoothStarter starter;
  {
    const std::lock_guard<std::mutex> lock(bluetoothMutex_);
    if (bluetoothOn_.load(std::memory_order_relaxed)) return BluetoothStart::kAlreadyOn;
    if (!bluetoothStarter_) return BluetoothStart::kNoStarter;
    // Taken, so it runs once per boot: a start that fails partway can't be undone, so the next
    // try is the restart. Not held across the start, which takes a while.
    starter = std::move(bluetoothStarter_);
    bluetoothStarter_ = nullptr;
  }
  if (!starter(key)) return BluetoothStart::kFailed;
  const std::lock_guard<std::mutex> lock(bluetoothMutex_);
  bluetoothKey_ = key;
  bluetoothOn_.store(true, std::memory_order_relaxed);
  return BluetoothStart::kStarted;
}

std::string RuntimeSettings::valueOf(const SettingSpec& spec) const {
  if (spec.name == "BLE_SECRET") {
    const std::lock_guard<std::mutex> lock(bluetoothMutex_);
    return bluetoothKey_;
  }
  if (spec.name == "BLE_NAME") return boot_.bleName;
  if (spec.name == "CUSTOM_BMS_CONFIG") return boot_.customBmsConfig ? "1" : "0";
  if (spec.name == "OBD_ENABLED") return boot_.obdEnabled ? "1" : "0";
  if (spec.name == "ELOCK_ENABLED") return boot_.elockEnabled ? "1" : "0";
  if (spec.name == "SERVICE_MODE_ENABLED") return boot_.serviceModeEnabled ? "1" : "0";
  if (spec.name == "SERVICE_WRITE_ENABLED") return boot_.serviceWriteEnabled ? "1" : "0";
  if (spec.name == "CHARGE_AUTO_ENABLED") return boot_.chargeAutoEnabled ? "1" : "0";
  if (spec.name == "WIFI_SSID") return boot_.wifiSsid;
  if (spec.name == "WIFI_PASSWORD") return boot_.wifiPassword;
  if (spec.name == "ADMIN_PASSWORD") return boot_.adminPassword;
  if (spec.name == "STATUS_REPORT") return statusReport() ? "1" : "0";
  if (spec.name == "STATUS_INTERVAL_S") return std::to_string(statusIntervalSeconds());
  return std::string(spec.defaultValue);
}

void RuntimeSettings::apply(const SettingSpec& spec, std::string_view normalizedValue) {
  if (!spec.live) return;
  if (spec.name == "STATUS_REPORT") {
    statusReport_.store(normalizedValue == "1", std::memory_order_relaxed);
  } else if (spec.name == "STATUS_INTERVAL_S") {
    if (const std::optional<std::int64_t> seconds = parseInteger(normalizedValue)) {
      statusIntervalSeconds_.store(static_cast<std::uint32_t>(*seconds), std::memory_order_relaxed);
    }
  }
}

namespace {

int runCommand(SettingsStorage& storage, RuntimeSettings& running, int argumentCount, const char* const* arguments,
               std::string& output) {
  const std::string_view command = argumentCount > 0 ? arguments[0] : "";
  if (command == "show") return show(storage, running, output);
  if (command == "set") return set(storage, running, argumentCount, arguments, output);
  if (command == "unset") return unset(storage, running, argumentCount, arguments, output);
  if (command == "status") return status(storage, running, argumentCount, arguments, output);
  appendLine(output, "unknown command");
  return 1;
}

}  // namespace

int runSettingsCommand(SettingsStorage& storage, RuntimeSettings& running, int argumentCount,
                       const char* const* arguments, std::string& output) {
  const std::lock_guard<std::mutex> lock(running.commandMutex());
  return runCommand(storage, running, argumentCount, arguments, output);
}

int runSettingsCommandAt(SettingsStorage& storage, RuntimeSettings& running, std::uint32_t expectedRevision,
                         int argumentCount, const char* const* arguments, std::string& output) {
  const std::lock_guard<std::mutex> lock(running.commandMutex());
  if (running.revision() != expectedRevision) {
    appendLine(output, "the settings changed since this page read them (another console or phone saved one); read "
                       "them again, then retry");
    return 2;
  }
  return runCommand(storage, running, argumentCount, arguments, output);
}

bool commandChangesSettings(int argumentCount, const char* const* arguments) {
  const std::string_view command = argumentCount > 0 ? arguments[0] : "";
  return command == "set" || command == "unset" || (command == "status" && argumentCount > 1);
}

StatusSchedule::Decision StatusSchedule::decide(const RuntimeSettings& running, bool reportRequested,
                                                std::int64_t nowMs) {
  if (!windowStartedMs_) {
    windowStartedMs_ = nowMs;
    wasOn_ = running.statusReport();
    return {reportRequested, true};
  }
  // An on-demand report covers the window so far and leaves the periodic cadence alone.
  if (reportRequested) {
    return {true, false};
  }
  const bool on = running.statusReport();
  if (on != wasOn_) {
    wasOn_ = on;
    windowStartedMs_ = nowMs;
    return {false, true};
  }
  if (!on || nowMs - *windowStartedMs_ < static_cast<std::int64_t>(running.statusIntervalSeconds()) * 1000) {
    return {false, false};
  }
  windowStartedMs_ = nowMs;
  return {true, true};
}

}  // namespace emcool::settings
