#include "can_bus.hpp"

#include <algorithm>
#include <cstring>

#include "emcool/log.hpp"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_twai_onchip.h"

namespace emcool {

namespace {

constexpr const char* kTag = "can";
constexpr std::uint32_t kBitrate = 500000;
// Roughly 100 ms of traffic, if cool-eva's candump volume (394 MB/h of text, ~2 000 frames/s
// by my estimate) is representative. A full queue drops the newest frame and counts it.
constexpr UBaseType_t kQueueDepth = 256;

const char* describeState(twai_error_state_t state) {
  switch (state) {
    case TWAI_ERROR_ACTIVE:
      return "error-active";
    case TWAI_ERROR_WARNING:
      return "error-warning";
    case TWAI_ERROR_PASSIVE:
      return "error-passive";
    case TWAI_ERROR_BUS_OFF:
      return "bus-off";
  }
  return "unknown";
}

}  // namespace

bool CanBus::start(int txGpio, int rxGpio, bool transmit) {
  transmitting_ = transmit;
  queue_ = xQueueCreate(kQueueDepth, sizeof(ReceivedFrame));
  if (queue_ == nullptr) {
    logError(kTag, "no memory for a %u-frame receive queue", static_cast<unsigned>(kQueueDepth));
    return false;
  }

  twai_onchip_node_config_t config = {};
  // TX is routed even though nothing is sent, so the controller holds the transceiver's TXD
  // recessive instead of leaving it to the pin's reset state, which could pull the bus dominant.
  config.io_cfg.tx = static_cast<gpio_num_t>(txGpio);
  config.io_cfg.rx = static_cast<gpio_num_t>(rxGpio);
  config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
  config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
  config.bit_timing.bitrate = kBitrate;
  config.flags.enable_listen_only = !transmit;
  config.flags.no_receive_rtr = true;
  // Retry until acknowledged, as SocketCAN does for cool-eva: the zeroed default is single-shot,
  // which would drop a request for losing arbitration to the bike's own traffic.
  config.fail_retry_cnt = -1;
  config.tx_queue_depth = kTransmitSlots;

  esp_err_t result = twai_new_node_onchip(&config, &node_);
  if (result != ESP_OK) {
    logError(kTag, "twai_new_node_onchip(tx=%d, rx=%d) failed: %s", txGpio, rxGpio, esp_err_to_name(result));
    return false;
  }

  // id 0 / mask 0 is the driver's "full open" filter. Set explicitly rather than trusting the
  // controller's reset state; FrameDecoder ignores ids it doesn't know.
  twai_mask_filter_config_t acceptEverything = {};
  acceptEverything.id = 0;
  acceptEverything.mask = 0;
  result = twai_node_config_mask_filter(node_, 0, &acceptEverything);
  if (result != ESP_OK) {
    logError(kTag, "configuring the accept-all filter failed: %s", esp_err_to_name(result));
    return false;
  }

  twai_event_callbacks_t callbacks = {};
  callbacks.on_rx_done = &CanBus::onReceive;
  callbacks.on_error = &CanBus::onError;
  callbacks.on_state_change = &CanBus::onStateChange;
  callbacks.on_tx_done = &CanBus::onTransmitted;
  result = twai_node_register_event_callbacks(node_, &callbacks, this);
  if (result != ESP_OK) {
    logError(kTag, "registering callbacks failed: %s", esp_err_to_name(result));
    return false;
  }

  result = twai_node_enable(node_);
  if (result != ESP_OK) {
    logError(kTag, "twai_node_enable failed: %s", esp_err_to_name(result));
    return false;
  }
  if (transmit) {
    logInfo(kTag, "on the bus at %u kbit/s, ACTIVE: acknowledging frames and sending OBD requests (tx=GPIO%d rx=GPIO%d)",
            static_cast<unsigned>(kBitrate / 1000), txGpio, rxGpio);
  } else {
    logInfo(kTag, "listening at %u kbit/s, listen-only (tx=GPIO%d rx=GPIO%d)", static_cast<unsigned>(kBitrate / 1000),
            txGpio, rxGpio);
  }
  return true;
}

std::optional<std::string> CanBus::transmit(std::uint32_t id, const obd::Payload& data) {
  if (!transmitting_) return std::string("the bus is listen-only");
  TransmitSlot* slot = nullptr;
  for (TransmitSlot& candidate : slots_) {
    if (!candidate.busy.load(std::memory_order_acquire)) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    counters_.sendRefused.fetch_add(1, std::memory_order_relaxed);
    return "all " + std::to_string(kTransmitSlots) + " transmit slots are waiting to go out (is anything acknowledging?)";
  }
  slot->data = data;
  slot->frame = {};
  slot->frame.header.id = id;
  slot->frame.header.dlc = 8;
  slot->frame.buffer = slot->data.data();
  slot->frame.buffer_len = slot->data.size();
  slot->busy.store(true, std::memory_order_release);
  const esp_err_t result = twai_node_transmit(node_, &slot->frame, 0);
  if (result != ESP_OK) {
    slot->busy.store(false, std::memory_order_release);
    counters_.sendRefused.fetch_add(1, std::memory_order_relaxed);
    return std::string("twai_node_transmit: ") + esp_err_to_name(result);
  }
  return std::nullopt;
}

void CanBus::recoverIfBusOff(std::int64_t nowMs) {
  if (!transmitting_ || !busOff_.load(std::memory_order_relaxed) || nowMs - lastRecoveryMs_ < 1000) return;
  lastRecoveryMs_ = nowMs;
  const esp_err_t result = twai_node_recover(node_);
  if (result == ESP_OK) {
    counters_.recoveries.fetch_add(1, std::memory_order_relaxed);
    logWarning(kTag, "the controller went bus-off; recovering");
  } else if (result == ESP_ERR_INVALID_STATE) {
    busOff_.store(false, std::memory_order_relaxed);
  } else {
    logWarning(kTag, "twai_node_recover failed: %s", esp_err_to_name(result));
  }
}

bool CanBus::receive(ReceivedFrame& frame, TickType_t timeout) {
  return xQueueReceive(queue_, &frame, timeout) == pdTRUE;
}

CanBusStatus CanBus::status() const {
  twai_node_status_t nodeStatus = {};
  const esp_err_t result = twai_node_get_info(node_, &nodeStatus, nullptr);
  if (result != ESP_OK) {
    logWarning(kTag, "twai_node_get_info failed: %s", esp_err_to_name(result));
    return {"unknown", 0};
  }
  return {describeState(nodeStatus.state), nodeStatus.rx_error_count};
}

bool IRAM_ATTR CanBus::onReceive(twai_node_handle_t node, const twai_rx_done_event_data_t*, void* context) {
  CanBus& bus = *static_cast<CanBus*>(context);
  std::uint8_t payload[8];
  twai_frame_t frame = {};
  frame.buffer = payload;
  frame.buffer_len = sizeof payload;
  if (twai_node_receive_from_isr(node, &frame) != ESP_OK) {
    return false;
  }
  if (frame.header.ide) {
    bus.counters_.ignoredExtended.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (frame.header.rtr) {
    bus.counters_.ignoredRemote.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ReceivedFrame received = {};
  received.arrivedUs = esp_timer_get_time();
  received.id = frame.header.id;
  // Classic CAN: a DLC of 9-15 still carries 8 bytes.
  received.length = static_cast<std::uint8_t>(std::min<unsigned>(frame.header.dlc, 8));
  std::memcpy(received.data, payload, received.length);
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  if (xQueueSendFromISR(bus.queue_, &received, &higherPriorityTaskWoken) != pdTRUE) {
    bus.counters_.droppedQueueFull.fetch_add(1, std::memory_order_relaxed);
  } else {
    bus.counters_.received.fetch_add(1, std::memory_order_relaxed);
  }
  return higherPriorityTaskWoken == pdTRUE;
}

bool IRAM_ATTR CanBus::onError(twai_node_handle_t, const twai_error_event_data_t*, void* context) {
  static_cast<CanBus*>(context)->counters_.busErrors.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool IRAM_ATTR CanBus::onStateChange(twai_node_handle_t, const twai_state_change_event_data_t* event, void* context) {
  CanBus& bus = *static_cast<CanBus*>(context);
  bus.counters_.stateChanges.fetch_add(1, std::memory_order_relaxed);
  bus.busOff_.store(event->new_sta == TWAI_ERROR_BUS_OFF, std::memory_order_relaxed);
  return false;
}

bool IRAM_ATTR CanBus::onTransmitted(twai_node_handle_t, const twai_tx_done_event_data_t* event, void* context) {
  CanBus& bus = *static_cast<CanBus*>(context);
  (event->is_tx_success ? bus.counters_.sent : bus.counters_.sendFailed).fetch_add(1, std::memory_order_relaxed);
  for (TransmitSlot& slot : bus.slots_) {
    if (&slot.frame == event->done_tx_frame) {
      slot.busy.store(false, std::memory_order_release);
      break;
    }
  }
  return false;
}

}  // namespace emcool
