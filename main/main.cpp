// emcool: listen to the bike's CAN bus, decode every frame into live state, log what changes
// to the microSD card, keep the clock on GPS time, serve the phone dashboard over Bluetooth,
// and report over the serial console every 10 s. With OBD_ENABLED, also poll OBD-II; otherwise
// it never sends. With WIFI_SSID and ADMIN_PASSWORD, join that Wi-Fi and serve the console and
// the ride log over it.
//
// Tasks: decode (core 1) owns the CAN queue, the clock and the ride log's segment buffers, and
// writes live state; diagnostics (core 1) owns everything that transmits — the OBD poller and
// service mode — fed the diagnostic replies by decode; ride-log (core 0) owns the card;
// ble-link (core 1) runs the phone link and reads live state; NimBLE's own host and controller
// run on core 0. Live state is shared under storeMutex.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ble_link.hpp"
#include "boot_guard.hpp"
#include "card_files.hpp"
#include "bootloader_random.h"
#include "can_bus.hpp"
#include "dashboard_console.hpp"
#include "emcool/ble/crypto.hpp"
#include "emcool/ble/session.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/ble_server/server.hpp"
#include "emcool/can/frame_decoder.hpp"
#include "emcool/can/pack_temperature.hpp"
#include "emcool/clock/system_clock_sync.hpp"
#include "emcool/log.hpp"
#include "diagnostics_task.hpp"
#include "emcool/obd/hub_diagnostics.hpp"
#include "emcool/obd/obd_stack.hpp"
#include "emcool/ride_log/download.hpp"
#include "emcool/ride_log/log_writer.hpp"
#include "emcool/ride_log/ride_log.hpp"
#include "emcool/settings/dashboard_reply.hpp"
#include "emcool/settings/settings.hpp"
#include "emcool/signals/signal_store.hpp"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ride_log_task.hpp"
#include "sd_card.hpp"
#include "sdkconfig.h"
#include "settings_console.hpp"
#include "status_report.hpp"
#include "system_time.hpp"
#include "wifi_link.hpp"

namespace {

constexpr const char* kTag = "emcool";
constexpr std::uint32_t kDecodeTaskStackBytes = 8192;
constexpr UBaseType_t kDecodeTaskPriority = 5;
// Core 1, leaving core 0 to the ride-log task and the Bluetooth stack.
constexpr BaseType_t kDecodeTaskCore = 1;

// The hub's fault list records into live state, stamped with its frame's arrival; the decode task
// holds no lock while it runs, so this takes storeMutex for each record.
class ArrivalRecorder final : public emcool::obd::SignalRecorder {
 public:
  ArrivalRecorder(emcool::signals::SignalStore& store, std::mutex& storeMutex)
      : store_(store), storeMutex_(storeMutex) {}
  void at(std::int64_t wallClockMs, std::int64_t monotonicMs) {
    wallClockMs_ = wallClockMs;
    monotonicMs_ = monotonicMs;
  }
  void record(std::string_view key, double value) override {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    store_.record(key, value, wallClockMs_, monotonicMs_);
  }

 private:
  emcool::signals::SignalStore& store_;
  std::mutex& storeMutex_;
  std::int64_t wallClockMs_ = 0;
  std::int64_t monotonicMs_ = 0;
};

struct DecodeContext {
  emcool::CanBus& bus;
  emcool::can::FrameDecoder& decoder;
  emcool::can::PackTemperatureRouter& packTemperature;
  emcool::signals::SignalStore& store;
  std::mutex& storeMutex;
  emcool::ride_log::RideLog& rideLog;
  emcool::clock::SystemClockSync& clock;
  emcool::StatusReport& status;
  emcool::DiagnosticsTask& diagnostics;
};

void decodeTask(void* argument);
void offerGpsTime(emcool::clock::SystemClockSync& clock, double gpsEpochSeconds);
emcool::ride_log::SessionId newSessionId();
bool initNvs();
template <typename Type, typename... Arguments>
Type* allocate(const char* what, Arguments&&... arguments);
// What Bluetooth needs to start, kept for a start after boot: `set BLE_SECRET` on a console.
struct BluetoothParts {
  const emcool::settings::Settings& settings;
  emcool::signals::SignalStore& store;
  std::mutex& storeMutex;
  emcool::ble_server::StorageReport& storage;
  emcool::DiagnosticsTask& diagnostics;
  emcool::ride_log::LogDownload* download;
  const emcool::ride_log::SessionId& session;
  emcool::BleLinkCounters& counters;
  emcool::ble_server::ConsoleEndpoints& console;
  bool afterBoot = false;
};
bool startBluetooth(const BluetoothParts& parts, const std::string& key);
std::string dashboardInstanceId();
std::string bluetoothCodeId(const std::string& key);
void logSettings(const emcool::settings::Settings& settings);

}  // namespace

extern "C" void app_main() {
  const esp_app_desc_t* app = esp_app_get_description();
  emcool::logInfo(kTag, "emcool %s, ESP-IDF %s", app->version, app->idf_ver);
  const bool safeMode = emcool::beginBoot();
  emcool::noteHeap("boot");
  const bool nvsReady = initNvs();
  // The settings, from NVS; defaults wherever nothing has been saved.
  static emcool::NvsSettingsStorage settingsStorage;
  static emcool::settings::RuntimeSettings runtimeSettings(emcool::settings::Settings::load(settingsStorage));
  const emcool::settings::Settings& settings = runtimeSettings.boot();
  logSettings(settings);
  // The console starts last, or alone: in safe mode, and when a step below fails, so a setting can
  // still be changed. Before, a failed step returned with no console at all.
  const auto startConsole = [] {
    emcool::startSettingsConsole(settingsStorage, runtimeSettings);
    emcool::noteHeap("the serial console");
    emcool::bootFinished();
  };
  if (safeMode) return startConsole();

  if (CONFIG_EMCOOL_CAN_TX_GPIO < 0 || CONFIG_EMCOOL_CAN_RX_GPIO < 0) {
    emcool::logError(kTag, "CAN pins are set to -1 (idf.py menuconfig → emcool), so the CAN bus stays off");
    return startConsole();
  }
  static emcool::CanBus bus;
  // Off the bus entirely (listen-only) unless something is meant to transmit.
  if (!bus.start(CONFIG_EMCOOL_CAN_TX_GPIO, CONFIG_EMCOOL_CAN_RX_GPIO, settings.obdEnabled)) {
    emcool::logError(kTag, "CAN bus did not start; nothing will be decoded");
    return startConsole();
  }
  emcool::noteHeap("the CAN bus");

  // The big buffers come from the heap, once: with Bluetooth on, the controller's reservation
  // leaves the static-data region too small for them, and the heap spans more of the RAM.
  static const emcool::ride_log::SessionId session = newSessionId();
  static emcool::clock::SystemClockSync clock;
  auto* segmentBuffers = new (std::nothrow) emcool::ride_log::SegmentBuffer[2];
  auto* writerScratch = new (std::nothrow) std::uint8_t[emcool::ride_log::kRecordCapacityBytes];
  if (segmentBuffers == nullptr || writerScratch == nullptr) {
    emcool::logError(kTag, "no memory for the ride log's buffers");
    return startConsole();
  }
  static emcool::FreeRtosSegmentQueue segmentQueue;
  if (!segmentQueue.create(2)) {
    emcool::logError(kTag, "no memory for the segment queue");
    return startConsole();
  }
  static emcool::FatCardSpace cardSpace;
  static emcool::ride_log::LogWriter writer({std::string(emcool::kSdMountPoint) + "/RIDES"}, session,
                                            std::span<std::uint8_t>(writerScratch, emcool::ride_log::kRecordCapacityBytes),
                                            cardSpace);
  static emcool::ride_log::RideLog rideLog(session, std::span<emcool::ride_log::SegmentBuffer>(segmentBuffers, 2),
                                           segmentQueue, clock);
  static emcool::SdCard card;
  static emcool::CardAccess cardAccess(card, writer);
  static emcool::RideLogTaskCounters rideLogTaskCounters;
  static emcool::RideLogTaskContext rideLogTaskContext{segmentQueue, writer, cardAccess, rideLogTaskCounters};
  // /dl reads the card from the Bluetooth task; with no card configured there is nothing to read.
  static emcool::ride_log::LogDownload logDownload(cardAccess);
  emcool::ride_log::LogDownload* download = emcool::startRideLogTask(rideLogTaskContext) ? &logDownload : nullptr;
  // The consoles' card commands (dir, cd, rm, cp, …), on the card the ride-log task mounts.
  static emcool::BikeCard bikeCard(cardAccess, writer, rideLogTaskCounters);
  if (download != nullptr) emcool::setCardFiles(&bikeCard);
  emcool::noteHeap("the ride log");

  auto* storeAllocation = allocate<emcool::signals::SignalStore>("live state", rideLog);
  if (storeAllocation == nullptr) return startConsole();
  emcool::signals::SignalStore& store = *storeAllocation;
  static std::mutex storeMutex;
  static emcool::can::FrameDecoder decoder;
  static emcool::can::PackTemperatureRouter packTemperature(settings.customBmsConfig);
  static emcool::BleLinkCounters bleLinkCounters;
  static emcool::RideLogStorageReport storageReport(writer, rideLogTaskCounters);
  // Everything that transmits, and service mode's snapshots on the card.
  auto* diagnostics = allocate<emcool::DiagnosticsTask>("the diagnostics task", bus, store, storeMutex, settings,
                                                        std::string(emcool::kSdMountPoint) + "/vcu-params");
  if (diagnostics == nullptr || !diagnostics->start()) return startConsole();
  emcool::noteHeap(settings.obdEnabled ? "live state and diagnostics (OBD on)" : "live state and diagnostics");
  // Settings → Bike console: the console's commands and status report over the Bluetooth link.
  static emcool::StatusText statusText;
  static emcool::DashboardConsoleHost consoleHost(statusText, store, storeMutex, *diagnostics);
  static emcool::files::CardShell bikeConsoleCard(bikeCard, emcool::cardFiles().gate, "Bike console",
                                                 emcool::ble_server::ConsoleEndpoints::kMaxOutputBytes);
  static emcool::ble_server::ConsoleEndpoints consoleEndpoints(settingsStorage, runtimeSettings, consoleHost,
                                                               download != nullptr ? &bikeConsoleCard : nullptr);
  // NimBLE keeps PHY calibration in NVS; without it the radio can't start.
  static BluetoothParts bluetoothParts{settings, store,           storeMutex,      storageReport,  *diagnostics,
                                       download, session,         bleLinkCounters, consoleEndpoints};
  const bool keyAtBoot = emcool::ble::BleSession::secretIsUsable(settings.bleSecret);
  if (!keyAtBoot) {
    emcool::logWarning(kTag, "Bluetooth is off: no Bluetooth key. On the serial console: set BLE_SECRET <key>, and "
                             "Bluetooth starts at once");
  }
  const bool bluetooth = nvsReady && keyAtBoot && startBluetooth(bluetoothParts, settings.bleSecret);
  if (bluetooth) emcool::noteHeap("Bluetooth");
  // Started at most once per boot: with no key at boot, the first `set BLE_SECRET` starts it.
  runtimeSettings.setBluetoothStarter(
      nvsReady && !keyAtBoot ? emcool::settings::RuntimeSettings::BluetoothStarter(
                                   [](const std::string& key) {
                                     bluetoothParts.afterBoot = true;
                                     return startBluetooth(bluetoothParts, key);
                                   })
                             : nullptr,
      bluetooth);
  // Wi-Fi after Bluetooth: they share the radio, and coexistence wants the Bluetooth controller up first.
  static emcool::WifiCounters wifiCounters;
  const bool wifi = nvsReady && emcool::startWifi(settings, settingsStorage, runtimeSettings,
                                                  download != nullptr ? &cardAccess : nullptr, wifiCounters);
  if (wifi) emcool::noteHeap("Wi-Fi");
  static emcool::StatusReport status({bus, store, storeMutex, rideLog, writer, rideLogTaskCounters, clock,
                                      bleLinkCounters, wifiCounters, wifi, runtimeSettings, diagnostics, statusText});
  static DecodeContext context{bus, decoder, packTemperature, store, storeMutex, rideLog, clock, status, *diagnostics};
  if (xTaskCreatePinnedToCore(decodeTask, "decode", kDecodeTaskStackBytes, &context, kDecodeTaskPriority, nullptr,
                              kDecodeTaskCore) != pdPASS) {
    emcool::logError(kTag, "could not create the decode task (%" PRIu32 " bytes of stack)", kDecodeTaskStackBytes);
  }
  startConsole();
}

namespace {

void decodeTask(void* argument) {
  DecodeContext& context = *static_cast<DecodeContext*>(argument);
  emcool::can::DecodedValues decoded;
  // The active fault list off 0x410, beside its decoding.
  static emcool::obd::HubDiagnostics hubDiagnostics;
  static ArrivalRecorder hubRecorder(context.store, context.storeMutex);

  while (true) {
    emcool::ReceivedFrame frame;
    if (context.bus.receive(frame, pdMS_TO_TICKS(1000))) {
      // The diagnostic replies are the diagnostics task's, and never decoded.
      if (emcool::obd::isDiagnosticReply(frame.id)) {
        context.diagnostics.forward(frame);
        continue;
      }
      decoded.clear();
      const emcool::can::FrameBytes data(frame.data, frame.length);
      if (frame.id == 0x410) {
        hubRecorder.at(emcool::wallClockMs(), emcool::monotonicMs());
        hubDiagnostics.handle(std::span<const std::uint8_t>(frame.data, frame.length), hubRecorder);
      }
      context.decoder.decode(frame.id, data, decoded);
      const std::int64_t arrivedMonotonicMs = emcool::monotonicMs();
      // Resolve the pack temperatures between decoding and recording.
      context.packTemperature.resolve(frame.id, data, decoded, arrivedMonotonicMs);
      context.status.noteFrame(decoded.size(), decoded.overflowed());
      const std::int64_t arrivedWallClockMs = emcool::wallClockMs();
      std::optional<double> gpsEpochSeconds;
      {
        const std::lock_guard<std::mutex> lock(context.storeMutex);
        for (const emcool::can::DecodedValue& value : decoded) {
          context.store.record(value.key, value.value, arrivedWallClockMs, arrivedMonotonicMs);
          if (std::string_view(value.key) == "gps_epoch_s") {
            gpsEpochSeconds = value.value;
          }
        }
      }
      if (gpsEpochSeconds) {
        offerGpsTime(context.clock, *gpsEpochSeconds);
      }
      // The charge-current ack-watch and the automatic controller see these as they arrive.
      // Outside storeMutex: observeChargeSignal takes the diagnostics task's own lock.
      for (const emcool::can::DecodedValue& value : decoded) {
        const std::string_view key(value.key);
        if (key == "fast_dc_target_a" || key == "charge_limit_a" || key == "charge_manager_state" ||
            key == "batt_temp_hi" || key == "soc" || key == "dc_charge_limit_selected_a") {
          context.diagnostics.observeChargeSignal(key, value.value, arrivedMonotonicMs);
        }
      }
    }
    const std::int64_t nowMs = emcool::monotonicMs();
    context.bus.recoverIfBusOff(nowMs);
    context.rideLog.tick(nowMs);
    context.status.reportIfDue(nowMs);
  }
}


// Steps the system clock with settimeofday when SystemClockSync says to.
void offerGpsTime(emcool::clock::SystemClockSync& clock, double gpsEpochSeconds) {
  const double monotonicMs = static_cast<double>(esp_timer_get_time()) / 1000.0;
  const double systemEpochSeconds = static_cast<double>(emcool::wallClockMs()) / 1000.0;
  const std::optional<emcool::clock::ClockVerdict> step = clock.offer(gpsEpochSeconds, systemEpochSeconds, monotonicMs);
  if (step) {
    clock.noteStepResult(emcool::stepWallClock(step->epochSeconds), step->offsetSeconds, step->reason);
  }
}

bool initNvs() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // An NVS partition this firmware can't read is no use to it either way: erasing it puts
    // every setting back to its default (and Bluetooth off, with no secret), loudly.
    emcool::logWarning(kTag, "NVS needs erasing (%s); erasing it, so every setting is back to its default",
                       esp_err_to_name(result));
    result = nvs_flash_erase();
    if (result == ESP_OK) result = nvs_flash_init();
  }
  if (result != ESP_OK) {
    emcool::logError(kTag, "NVS did not initialise (%s); Bluetooth stays off", esp_err_to_name(result));
    return false;
  }
  return true;
}

// The phone link with `key`, if it is usable and the crypto passes its known-answer tests: at boot
// with the saved key, or later from a console's `set BLE_SECRET`, at most once per boot.
bool startBluetooth(const BluetoothParts& parts, const std::string& key) {
  if (!emcool::ble::BleSession::secretIsUsable(key)) {
    emcool::logWarning(kTag, "Bluetooth is off: the Bluetooth key isn't usable");
    return false;
  }
  if (!emcool::ble::initCrypto() || !emcool::ble::cryptoSelfTest()) {
    emcool::logError(kTag, "Bluetooth is off: the crypto failed its self-test");
    return false;
  }
  const std::string instanceId = dashboardInstanceId();
  const std::string codeId = bluetoothCodeId(key);
  std::string settingsReply = emcool::settings::dashboardSettingsReply(
      parts.settings,
      {esp_app_get_description()->version, parts.session.hex.data(), instanceId, parts.download != nullptr, codeId});
  auto* endpoints = allocate<emcool::ble_server::DashboardEndpoints>(
      "the dashboard endpoints", parts.store, parts.storeMutex, parts.storage, parts.diagnostics, &parts.diagnostics,
      parts.download, std::move(settingsReply), &parts.console);
  auto* server = endpoints == nullptr ? nullptr
                                      : allocate<emcool::ble_server::BleServer>("the Bluetooth server", key,
                                                                                 parts.store, parts.storeMutex,
                                                                                 *endpoints);
  const bool started =
      server != nullptr && emcool::startBleLink(*server, parts.settings.bleName.c_str(), parts.counters);
  // A start from a console after boot: the boot log's last heap line no longer covers it.
  if (started && parts.afterBoot) emcool::noteHeap("Bluetooth");
  return started;
}

// What this boot runs with — the secret only by its length — and the BMS line.
void logSettings(const emcool::settings::Settings& settings) {
  emcool::logInfo(kTag,
                  "settings: BLE_SECRET %s, BLE_NAME %s, CUSTOM_BMS_CONFIG %d, OBD_ENABLED %d, ELOCK_ENABLED %d, "
                  "SERVICE_MODE_ENABLED %d, WIFI_SSID %s, status report %s (serial console: show)",
                  settings.bleSecret.empty() ? "not set" : "set",
                  settings.bleName.c_str(), settings.customBmsConfig ? 1 : 0, settings.obdEnabled ? 1 : 0,
                  settings.elockEnabled ? 1 : 0, settings.serviceModeEnabled ? 1 : 0,
                  settings.wifiSsid.empty() ? "not set" : settings.wifiSsid.c_str(),
                  settings.statusReport ? ("every " + std::to_string(settings.statusIntervalSeconds) + " s").c_str()
                                        : "off");
  if (!settings.obdEnabled) {
    emcool::logInfo("obd", "disabled (OBD_ENABLED=0) — passive decode only, listen-only on the bus");
    if (settings.elockEnabled) emcool::logWarning("elock", "ELOCK_ENABLED is 1 but needs OBD_ENABLED too; not reading");
  } else if (!settings.elockEnabled) {
    emcool::logInfo("elock", "disabled (ELOCK_ENABLED=0)");
  }
  emcool::logInfo("bms", "%s",
                  settings.customBmsConfig
                      ? "custom config expected — true pack temps from 0x660, 0x200 logged as batt_temp_*_vcu"
                      : "stock config assumed — pack temps from 0x200 once no 0x660 has been seen (set "
                        "CUSTOM_BMS_CONFIG 1 if flashed)");
}

// The dashboard keys its per-bike state on this, so it must not change across boots: the factory
// MAC, in the dashboard's instance-id shape (a UUID's 8-4-4-4-12), zero-padded in front.
std::string dashboardInstanceId() {
  std::uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    emcool::logWarning(kTag, "could not read the factory MAC; the dashboard's instance id is all zeros");
  }
  char id[37];
  std::snprintf(id, sizeof id, "00000000-0000-0000-0000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
  return id;
}

// The dashboard files its saved copies of the key under this, and expects a UUID. A labelled
// HMAC of the key: it changes with the key and reveals nothing of it. Empty if the HMAC fails.
std::string bluetoothCodeId(const std::string& key) {
  constexpr std::string_view kLabel = "emcool/code-id";
  emcool::ble::Sha256Digest digest = {};
  if (!emcool::ble::hmacSha256(
          std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()),
          {std::span(reinterpret_cast<const std::uint8_t*>(kLabel.data()), kLabel.size())}, digest)) {
    emcool::logWarning(kTag, "could not derive the Bluetooth key's id; the dashboard will say no key is saved");
    return {};
  }
  char id[37];
  std::snprintf(id, sizeof id, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", digest[0],
                digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], digest[8], digest[9],
                digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
  return id;
}

// For the objects that live as long as the firmware and are too big for static data.
template <typename Type, typename... Arguments>
Type* allocate(const char* what, Arguments&&... arguments) {
  Type* object = new (std::nothrow) Type(std::forward<Arguments>(arguments)...);
  if (object == nullptr) {
    emcool::logError(kTag, "no memory for %s (%zu bytes)", what, sizeof(Type));
  }
  return object;
}

// Before the radio is up, esp_random has no RF noise to draw on; the bootloader's entropy
// source fills in for the eight bytes a session id needs.
emcool::ride_log::SessionId newSessionId() {
  std::array<std::uint8_t, 8> bytes = {};
  bootloader_random_enable();
  esp_fill_random(bytes.data(), bytes.size());
  bootloader_random_disable();
  return emcool::ride_log::SessionId::fromBytes(bytes);
}

}  // namespace
