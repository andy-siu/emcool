#pragma once

// The settings on the device: NVS storage under the "emcool" namespace, and the serial
// console that changes them — `idf.py monitor`, then `show`, `set NAME VALUE`, `unset NAME`,
// `status [on | off | every SECONDS]`, `new-ble-key [replace]`, `restart`, `help`. A secret is
// never printed back, only its length — except the Bluetooth key, shown once as it is generated.

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "emcool/settings/settings.hpp"

namespace emcool {

class NvsSettingsStorage final : public settings::SettingsStorage {
 public:
  std::optional<std::string> read(std::string_view key) override;
  bool write(std::string_view key, std::string_view value) override;
  bool erase(std::string_view key) override;
};

// Starts the console's own task on the UART the logs go to. `running` must outlive it. `radioOn`:
// Bluetooth or Wi-Fi is up, so the hardware RNG is drawing on RF noise.
bool startSettingsConsole(settings::SettingsStorage& storage, settings::RuntimeSettings& running, bool radioOn);

}  // namespace emcool
