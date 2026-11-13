#include "settings_console.hpp"

#include <cstdio>

#include "emcool/log.hpp"
#include "esp_console.h"
#include "esp_system.h"
#include "nvs.h"
#include "wifi_link.hpp"

// The console follows ESP-IDF's examples/system/console/basic.

namespace emcool {

namespace {

constexpr const char* kTag = "settings";
constexpr const char* kNamespace = "emcool";

settings::SettingsStorage* consoleStorage = nullptr;
settings::RuntimeSettings* runningSettings = nullptr;

int runCommand(int argumentCount, char** arguments) {
  std::string output;
  const int status = settings::runSettingsCommand(*consoleStorage, *runningSettings, argumentCount, arguments, output);
  std::fputs(output.c_str(), stdout);
  return status;
}

int network(int, char**) {
  std::fputs(describeNetwork().c_str(), stdout);
  return 0;
}

int restart(int, char**) {
  std::puts("restarting");
  esp_restart();
  return 0;
}

std::string helpText() {
  std::string text = "Settings, kept in NVS:";
  for (const settings::SettingSpec& spec : settings::settingSpecs()) {
    text += "\n  ";
    text += spec.name;
    text += ": ";
    text += spec.help;
  }
  return text;
}

}  // namespace

std::optional<std::string> NvsSettingsStorage::read(std::string_view key) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return std::nullopt;  // nothing has ever been saved
  if (result != ESP_OK) {
    logWarning(kTag, "nvs_open failed: %s", esp_err_to_name(result));
    return std::nullopt;
  }
  const std::string name(key);
  std::size_t length = 0;
  result = nvs_get_str(handle, name.c_str(), nullptr, &length);
  std::optional<std::string> value;
  if (result == ESP_OK && length > 0) {
    std::string buffer(length, '\0');
    result = nvs_get_str(handle, name.c_str(), buffer.data(), &length);
    if (result == ESP_OK) {
      buffer.resize(length - 1);  // the stored length counts the terminator
      value = std::move(buffer);
    }
  }
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    logWarning(kTag, "reading %s failed: %s", name.c_str(), esp_err_to_name(result));
  }
  nvs_close(handle);
  return value;
}

bool NvsSettingsStorage::write(std::string_view key, std::string_view value) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) {
    logError(kTag, "nvs_open failed: %s", esp_err_to_name(result));
    return false;
  }
  const std::string name(key);
  result = nvs_set_str(handle, name.c_str(), std::string(value).c_str());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    logError(kTag, "saving %s failed: %s", name.c_str(), esp_err_to_name(result));
    return false;
  }
  return true;
}

bool NvsSettingsStorage::erase(std::string_view key) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) {
    logError(kTag, "nvs_open failed: %s", esp_err_to_name(result));
    return false;
  }
  const std::string name(key);
  result = nvs_erase_key(handle, name.c_str());
  if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;  // already at its default
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    logError(kTag, "removing %s failed: %s", name.c_str(), esp_err_to_name(result));
    return false;
  }
  return true;
}

bool startSettingsConsole(settings::SettingsStorage& storage, settings::RuntimeSettings& running) {
  consoleStorage = &storage;
  runningSettings = &running;

  static const std::string settingsHelp = helpText();
  const esp_console_cmd_t commands[] = {
      {.command = "show", .help = settingsHelp.c_str(), .hint = nullptr, .func = runCommand, .argtable = nullptr,
       .func_w_context = nullptr, .context = nullptr},
      {.command = "set",
       .help = "Save a setting: set NAME VALUE. Quote a value with spaces. `set BLE_SECRET <key>` turns Bluetooth on "
               "at once.",
       .hint = "NAME VALUE", .func = runCommand, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "unset", .help = "Put a setting back to its default.", .hint = "NAME", .func = runCommand,
       .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "status",
       .help = "The periodic status report: `status` prints one now; `status off`, `status on` and `status every "
               "SECONDS` (5-3600) change it at once and are saved.",
       .hint = "[on | off | every SECONDS]", .func = runCommand, .argtable = nullptr, .func_w_context = nullptr,
       .context = nullptr},
      {.command = "network",
       .help = "The Wi-Fi network joined and its signal, and the address, netmask, gateway and DNS server it handed "
               "out.",
       .hint = nullptr, .func = network, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "restart", .help = "Restart, so saved settings take effect.", .hint = nullptr, .func = restart,
       .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
  };

  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  replConfig.prompt = "emcool> ";
  replConfig.task_stack_size = 6144;
  // History lives only in RAM, but a typed secret would sit in it until the next restart.
  replConfig.max_history_len = 8;
  esp_console_dev_uart_config_t uartConfig = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  esp_err_t result = esp_console_new_repl_uart(&uartConfig, &replConfig, &repl);
  if (result != ESP_OK) {
    logError(kTag, "the serial console did not start: %s", esp_err_to_name(result));
    return false;
  }
  for (const esp_console_cmd_t& command : commands) {
    result = esp_console_cmd_register(&command);
    if (result != ESP_OK) {
      logError(kTag, "registering `%s` failed: %s", command.command, esp_err_to_name(result));
      return false;
    }
  }
  esp_console_register_help_command();
  result = esp_console_start_repl(repl);
  if (result != ESP_OK) {
    logError(kTag, "the serial console did not start: %s", esp_err_to_name(result));
    return false;
  }
  return true;
}

}  // namespace emcool
