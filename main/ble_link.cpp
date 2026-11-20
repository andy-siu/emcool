#include "ble_link.hpp"

#include <cstring>

#include "emcool/ble/wire.hpp"
#include "emcool/log.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "system_time.hpp"

// Follows ESP-IDF's examples/bluetooth/nimble/bleprph.

namespace emcool {

namespace {

constexpr const char* kTag = "ble";
constexpr std::uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 3;
// Core 1 with the decode task, which outranks it; NimBLE's host and controller run on core 0.
constexpr BaseType_t kTaskCore = 1;
// How often the server's timers run when nothing else wakes the task: under its 100 ms patch window.
constexpr TickType_t kTickPeriod = pdMS_TO_TICKS(10);
// Asked of the phone once it connects: a 15-30 ms interval and a 5 s supervision timeout, so a radio
// shared with Wi-Fi (a scan, a join) or a busy moment on core 0 can miss a few connection events
// without the link being dropped. Phones often start with 0.7-2 s. Within Apple's accessory rules
// (timeout 2-6 s, interval max at least min + 15 ms, max x (latency + 1) x 3 under the timeout); a
// phone that won't keeps its own, which the log shows.
constexpr std::uint16_t kIntervalMinUnits = 12;          // 1.25 ms units: 15 ms
constexpr std::uint16_t kIntervalMaxUnits = 24;          // 30 ms
constexpr std::uint16_t kSupervisionTimeoutUnits = 500;  // 10 ms units: 5 s
constexpr UBaseType_t kEventQueueDepth = 8;

enum class EventType : std::uint8_t { kConnected, kDisconnected, kNotifications, kMtu, kWrite };

struct LinkEvent {
  EventType type;
  std::uint16_t value;
  std::uint16_t length;
  std::uint8_t data[ble::kMaxWriteFrameBytes];
};

struct LinkState {
  ble_server::BleServer* server = nullptr;
  BleLinkCounters* counters = nullptr;
  const char* deviceName = nullptr;
  QueueHandle_t events = nullptr;
  std::uint8_t ownAddressType = 0;
  // Written by NimBLE's host task, read by the link task.
  std::atomic<std::uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
  std::uint16_t txHandle = 0;
  std::mutex infoMutex;
  char info[96] = {};
  std::size_t infoLength = 0;
};

LinkState link;

ble_uuid_any_t serviceUuid;
ble_uuid_any_t rxUuid;
ble_uuid_any_t txUuid;
ble_uuid_any_t infoUuid;

int accessCharacteristic(std::uint16_t connection, std::uint16_t attribute, ble_gatt_access_ctxt* context, void* argument);
int gapEvent(ble_gap_event* event, void* argument);
void advertise();
void onSync();
void onReset(int reason);
void hostTask(void* argument);
void linkTask(void* argument);
void post(EventType type, std::uint16_t value);
void refreshInfo();

// Filled in by defineService(); NimBLE wants a zeroed entry to end each list.
ble_gatt_chr_def characteristics[4] = {};
ble_gatt_svc_def services[2] = {};

// rx (phone → bike, written with response), tx (bike → phone, notify), info (read).
void defineService() {
  characteristics[0].uuid = &rxUuid.u;
  characteristics[0].access_cb = accessCharacteristic;
  characteristics[0].flags = BLE_GATT_CHR_F_WRITE;
  characteristics[1].uuid = &txUuid.u;
  characteristics[1].access_cb = accessCharacteristic;
  characteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
  characteristics[1].val_handle = &link.txHandle;
  characteristics[2].uuid = &infoUuid.u;
  characteristics[2].access_cb = accessCharacteristic;
  characteristics[2].flags = BLE_GATT_CHR_F_READ;
  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &serviceUuid.u;
  services[0].characteristics = characteristics;
}

int accessCharacteristic(std::uint16_t, std::uint16_t, ble_gatt_access_ctxt* context, void*) {
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(context->chr->uuid, &infoUuid.u) == 0) {
    const std::lock_guard<std::mutex> lock(link.infoMutex);
    return os_mbuf_append(context->om, link.info, link.infoLength) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(context->chr->uuid, &rxUuid.u) == 0) {
    LinkEvent event = {};
    event.type = EventType::kWrite;
    std::uint16_t length = 0;
    if (ble_hs_mbuf_to_flat(context->om, event.data, sizeof event.data, &length) != 0) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    event.length = length;
    if (xQueueSend(link.events, &event, 0) != pdTRUE) {
      // The phone writes with response, one frame at a time, so a full queue means this
      // task is wedged; failing the write tells the phone instead of losing it silently.
      link.counters->writesDropped.fetch_add(1, std::memory_order_relaxed);
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

// Why a link ended, in words, for the reasons that come up. NimBLE's codes are its own for the
// host's, and 0x200 plus the Bluetooth spec's for the controller's.
const char* disconnectReason(int reason) {
  switch (reason) {
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):
      return "supervision timeout: the radio lost the phone (range, interference, or the radio busy elsewhere)";
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM):
      return "the phone ended it";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL):
      return "emcool ended it";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT):
      return "the connection never got going";
    case BLE_HS_HCI_ERR(BLE_ERR_LMP_LL_RSP_TMO):
      return "the phone stopped answering link-layer requests";
    case BLE_HS_HCI_ERR(BLE_ERR_INSTANT_PASSED):
      return "a parameter or channel change was missed";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_MIC):
      return "a packet failed its integrity check";
    default:
      return "see NimBLE's error codes";
  }
}

// The interval, latency and supervision timeout the link runs with now.
void logConnectionParameters(std::uint16_t connection, const char* when) {
  ble_gap_conn_desc description = {};
  if (ble_gap_conn_find(connection, &description) != 0) return;
  // In whole hundredths of a millisecond: no %f on NimBLE's host task, whose stack is 4 KB.
  const unsigned hundredths = static_cast<unsigned>(description.conn_itvl) * 125;
  logInfo(kTag, "%s: interval %u.%02u ms, latency %u, supervision timeout %u ms", when, hundredths / 100,
          hundredths % 100, static_cast<unsigned>(description.conn_latency),
          static_cast<unsigned>(description.supervision_timeout) * 10);
}

void askForLongerTimeout(std::uint16_t connection) {
  ble_gap_upd_params parameters = {};
  parameters.itvl_min = kIntervalMinUnits;
  parameters.itvl_max = kIntervalMaxUnits;
  parameters.latency = 0;
  parameters.supervision_timeout = kSupervisionTimeoutUnits;
  const int result = ble_gap_update_params(connection, &parameters);
  if (result != 0) logWarning(kTag, "asking the phone for a 5 s supervision timeout failed (%d)", result);
}

int gapEvent(ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        link.connection.store(event->connect.conn_handle);
        post(EventType::kConnected, 0);
        logConnectionParameters(event->connect.conn_handle, "link parameters at connect");
        askForLongerTimeout(event->connect.conn_handle);
      } else {
        advertise();
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      logWarning(kTag, "the link ended: reason 0x%03x, %s", static_cast<unsigned>(event->disconnect.reason),
                 disconnectReason(event->disconnect.reason));
      link.connection.store(BLE_HS_CONN_HANDLE_NONE);
      post(EventType::kDisconnected, 0);
      advertise();
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
      if (event->conn_update.status == 0) {
        logConnectionParameters(event->conn_update.conn_handle, "link parameters now");
      } else {
        logInfo(kTag, "the phone kept its link parameters (%d)", event->conn_update.status);
      }
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == link.txHandle) {
        post(EventType::kNotifications, event->subscribe.cur_notify);
      }
      return 0;
    case BLE_GAP_EVENT_MTU:
      post(EventType::kMtu, event->mtu.value);
      return 0;
    default:
      return 0;
  }
}

// Advertising carries the flags and the dashboard's service UUID — what the phone filters on; the
// name goes in the scan response, since 128-bit UUID and name together overrun 31 bytes.
void advertise() {
  ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &serviceUuid.u128;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int result = ble_gap_adv_set_fields(&fields);
  if (result != 0) {
    logError(kTag, "setting the advertisement failed (%d)", result);
    return;
  }
  ble_hs_adv_fields response = {};
  response.name = reinterpret_cast<const std::uint8_t*>(link.deviceName);
  response.name_len = static_cast<std::uint8_t>(std::strlen(link.deviceName));
  response.name_is_complete = 1;
  result = ble_gap_adv_rsp_set_fields(&response);
  if (result != 0) {
    logError(kTag, "setting the scan response failed (%d)", result);
    return;
  }
  ble_gap_adv_params parameters = {};
  parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
  parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
  result = ble_gap_adv_start(link.ownAddressType, nullptr, BLE_HS_FOREVER, &parameters, gapEvent, nullptr);
  if (result != 0 && result != BLE_HS_EALREADY) {
    logError(kTag, "starting to advertise failed (%d)", result);
  }
}

void onSync() {
  int result = ble_hs_util_ensure_addr(0);
  if (result == 0) result = ble_hs_id_infer_auto(0, &link.ownAddressType);
  if (result != 0) {
    logError(kTag, "no usable Bluetooth address (%d)", result);
    return;
  }
  logInfo(kTag, "advertising as \"%s\" (service %.*s)", link.deviceName, static_cast<int>(ble::kServiceUuid.size()),
          ble::kServiceUuid.data());
  advertise();
}

void onReset(int reason) { logWarning(kTag, "the Bluetooth host reset (reason %d)", reason); }

void hostTask(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void post(EventType type, std::uint16_t value) {
  LinkEvent event = {};
  event.type = type;
  event.value = value;
  if (xQueueSend(link.events, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
    logError(kTag, "link event %u lost: the link task isn't keeping up", static_cast<unsigned>(type));
  }
}

void refreshInfo() {
  char info[sizeof link.info];
  const std::size_t length = link.server->info(info);
  const std::lock_guard<std::mutex> lock(link.infoMutex);
  std::memcpy(link.info, info, length);
  link.infoLength = length;
}

void handle(const LinkEvent& event) {
  ble_server::BleServer& server = *link.server;
  switch (event.type) {
    case EventType::kConnected:
      server.connected();
      link.counters->connected.store(true, std::memory_order_relaxed);
      link.counters->mtu.store(23, std::memory_order_relaxed);
      logInfo(kTag, "phone connected");
      break;
    case EventType::kDisconnected:
      server.disconnected();
      link.counters->connected.store(false, std::memory_order_relaxed);
      logInfo(kTag, "phone disconnected");
      break;
    case EventType::kNotifications:
      server.notificationsEnabled(event.value != 0);
      break;
    case EventType::kMtu:
      server.mtuChanged(event.value);
      link.counters->mtu.store(event.value, std::memory_order_relaxed);
      break;
    case EventType::kWrite:
      server.receiveFrame(std::span<const std::uint8_t>(event.data, event.length), monotonicMs());
      break;
  }
}

// NimBLE has real flow control: when its buffers are full there is no mbuf to be had, and the
// frame waits for the next pass. That is the whole of the pacing.
void sendFrames(ble_server::BleServer& server) {
  std::uint8_t frame[ble::kMaxWriteFrameBytes];
  std::size_t length = 0;
  while (link.connection.load() != BLE_HS_CONN_HANDLE_NONE && server.nextFrame(frame, length)) {
    os_mbuf* buffer = ble_hs_mbuf_from_flat(frame, static_cast<std::uint16_t>(length));
    if (buffer == nullptr) return;
    // ble_gatts_notify_custom consumes the mbuf whatever it returns.
    const int result = ble_gatts_notify_custom(link.connection.load(), link.txHandle, buffer);
    if (result != 0) {
      link.counters->notifyFailures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    server.frameSent();
    server.tick(monotonicMs(), wallClockMs());
  }
}

void linkTask(void*) {
  LinkEvent event;
  while (true) {
    if (xQueueReceive(link.events, &event, kTickPeriod) == pdTRUE) {
      handle(event);
      while (xQueueReceive(link.events, &event, 0) == pdTRUE) {
        handle(event);
      }
      refreshInfo();
    }
    link.server->tick(monotonicMs(), wallClockMs());
    sendFrames(*link.server);
    link.counters->authenticated.store(link.server->authenticated(), std::memory_order_relaxed);
    link.counters->framesSent.store(link.server->counters().framesSent, std::memory_order_relaxed);
  }
}

}  // namespace

bool startBleLink(ble_server::BleServer& server, const char* deviceName, BleLinkCounters& counters) {
  link.server = &server;
  link.counters = &counters;
  link.deviceName = deviceName;
  if (ble_uuid_from_str(&serviceUuid, ble::kServiceUuid.data()) != 0 ||
      ble_uuid_from_str(&rxUuid, ble::kRxCharacteristicUuid.data()) != 0 ||
      ble_uuid_from_str(&txUuid, ble::kTxCharacteristicUuid.data()) != 0 ||
      ble_uuid_from_str(&infoUuid, ble::kInfoCharacteristicUuid.data()) != 0) {
    logError(kTag, "could not parse the service UUIDs");
    return false;
  }
  link.events = xQueueCreate(kEventQueueDepth, sizeof(LinkEvent));
  if (link.events == nullptr) {
    logError(kTag, "no memory for the link event queue");
    return false;
  }
  refreshInfo();

  const esp_err_t initialised = nimble_port_init();
  if (initialised != ESP_OK) {
    logError(kTag, "nimble_port_init failed: %s", esp_err_to_name(initialised));
    return false;
  }
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.reset_cb = onReset;
  defineService();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int result = ble_gatts_count_cfg(services);
  if (result == 0) result = ble_gatts_add_svcs(services);
  if (result == 0) result = ble_svc_gap_device_name_set(deviceName);
  if (result != 0) {
    logError(kTag, "registering the GATT service failed (%d)", result);
    return false;
  }
  if (xTaskCreatePinnedToCore(linkTask, "ble-link", kTaskStackBytes, nullptr, kTaskPriority, nullptr, kTaskCore) !=
      pdPASS) {
    logError(kTag, "could not create the link task");
    return false;
  }
  nimble_port_freertos_init(hostTask);
  return true;
}

}  // namespace emcool
