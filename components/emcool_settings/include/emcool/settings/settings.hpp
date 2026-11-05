#pragma once

// The per-bike settings: what cool-eva keeps in /etc/default/cool-eva, under the same names
// where cool-eva has one. Kept in the ESP32's NVS, so they survive reflashing and firmware
// updates (only `idf.py erase-flash` clears them), and set from the serial console.
// Most are read once at boot, so a change takes effect at the next restart and `show` says
// when one is waiting; the few marked live (the status report's) apply the moment they're set.
// Only settings for what emcool actually does are here; cool-eva's others return with the
// features they configure.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace emcool::settings {

enum class SettingType { kSecret, kText, kFlag, kInteger };

struct SettingSpec {
  std::string_view name;
  // NVS keys are at most 15 characters, so the stored key is not always the name.
  std::string_view storageKey;
  SettingType type;
  std::string_view defaultValue;
  // The allowed length of a text or secret, or the allowed range of an integer.
  std::int64_t minimum;
  std::int64_t maximum;
  // Applied as soon as it's set, rather than at the next restart.
  bool live;
  std::string_view help;
};

std::span<const SettingSpec> settingSpecs();
// Case-insensitive, so `set custom_bms_config 1` works too.
const SettingSpec* findSetting(std::string_view name);

// Why `value` can't be stored for `spec`, or nullopt if it can. Flags take 1/0, on/off,
// true/false or yes/no; normalizeValue() turns any of those into "1" or "0".
std::optional<std::string> validate(const SettingSpec& spec, std::string_view value);
std::string normalizeValue(const SettingSpec& spec, std::string_view value);

class SettingsStorage {
 public:
  virtual ~SettingsStorage() = default;
  // nullopt when the key isn't stored, or couldn't be read.
  virtual std::optional<std::string> read(std::string_view key) = 0;
  virtual bool write(std::string_view key, std::string_view value) = 0;
  virtual bool erase(std::string_view key) = 0;
};

// The values read at boot.
struct Settings {
  std::string bleSecret;
  std::string bleName;
  bool customBmsConfig = false;
  // Transmitting: off unless set. The E-LOCK read happens only with OBD on, as in cool-eva.
  bool obdEnabled = false;
  bool elockEnabled = false;
  // Service mode's VCU reads, which also need OBD_ENABLED for an active bus.
  bool serviceModeEnabled = false;
  // Service mode's VCU WRITES (/vcu-write). The only transmitting switch off by default on purpose.
  bool serviceWriteEnabled = false;
  // The automatic charge-current controller. Needs SERVICE_WRITE_ENABLED, since it commands writes.
  bool chargeAutoEnabled = false;
  // Wi-Fi joins wifiSsid, and only with an admin password for what it serves (wifiEnabled()).
  std::string wifiSsid;
  std::string wifiPassword;
  std::string adminPassword;
  bool statusReport = true;
  std::uint32_t statusIntervalSeconds = 10;

  bool wifiEnabled() const { return !wifiSsid.empty() && !adminPassword.empty(); }

  // A stored value that no longer validates is ignored (with a warning) and the default used.
  static Settings load(SettingsStorage& storage);
};

// What the firmware is running with: the boot values, plus the live settings, which the
// console changes while other tasks read them.
class RuntimeSettings {
 public:
  explicit RuntimeSettings(Settings boot);

  const Settings& boot() const { return boot_; }
  bool statusReport() const { return statusReport_.load(std::memory_order_relaxed); }
  std::uint32_t statusIntervalSeconds() const { return statusIntervalSeconds_.load(std::memory_order_relaxed); }

  // `status` with no arguments: a report as soon as the reporting task next looks.
  void requestReport() { reportRequested_.store(true, std::memory_order_relaxed); }
  bool takeReportRequest() { return reportRequested_.exchange(false, std::memory_order_relaxed); }

  // The value in force now, as `show` prints it.
  std::string valueOf(const SettingSpec& spec) const;
  // Puts a live setting into force; a value that isn't live waits for the restart.
  void apply(const SettingSpec& spec, std::string_view normalizedValue);

 private:
  Settings boot_;
  std::atomic<bool> statusReport_;
  std::atomic<std::uint32_t> statusIntervalSeconds_;
  std::atomic<bool> reportRequested_{false};
};

// Fills a buffer with cryptographically random bytes; the firmware's draws on the hardware RNG.
using RandomFill = std::function<void(std::span<std::uint8_t>)>;

// The console's commands, writing what to print into `output`:
//   show | set NAME VALUE | unset NAME | status [on | off | every SECONDS] | new-ble-key [replace]
// Returns 0 on success, 1 on a usage or storage error, as an esp_console command does.
// `new-ble-key` needs `fillRandom`, and fails without one.
int runSettingsCommand(SettingsStorage& storage, RuntimeSettings& running, int argumentCount,
                       const char* const* arguments, std::string& output, const RandomFill& fillRandom = nullptr);

// When the periodic status report is due. Each periodic report covers the time since the last
// window began. Turning reporting on or off starts a fresh window, so the first report after
// `status on` covers only the time since; an on-demand report (`status`) covers the window so far
// and doesn't move it.
class StatusSchedule {
 public:
  struct Decision {
    bool report;
    // Start a new window now (the caller resets its counters), whether or not it reports.
    bool restartWindow;
  };

  Decision decide(const RuntimeSettings& running, bool reportRequested, std::int64_t nowMs);

 private:
  std::optional<std::int64_t> windowStartedMs_;
  bool wasOn_ = false;
};

}  // namespace emcool::settings
