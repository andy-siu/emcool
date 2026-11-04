#pragma once

// The bike's CAN bus through the on-chip TWAI controller: 500 kbit, 11-bit. Frames arrive in
// an interrupt callback and are queued for a task to decode.
//
// LISTEN-ONLY unless a transmitting setting is on (OBD_ENABLED): no transmission of any kind —
// not even the ACK bit or an error frame — so the bike's bus cannot tell this node is there.
// With transmitting on, the controller is an ordinary, ACTIVE node: it acknowledges every
// frame, and sends what the OBD side asks it to, retrying a frame until it is acknowledged.

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "emcool/obd/bus.hpp"
#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace emcool {

struct ReceivedFrame {
  std::uint32_t id;
  std::uint8_t length;
  std::uint8_t data[8];
  // When the interrupt took it, on the monotonic clock: how late our flow control was is measured
  // from here.
  std::int64_t arrivedUs;
};

struct CanBusCounters {
  std::atomic<std::uint32_t> received{0};
  std::atomic<std::uint32_t> droppedQueueFull{0};
  std::atomic<std::uint32_t> ignoredExtended{0};
  std::atomic<std::uint32_t> ignoredRemote{0};
  std::atomic<std::uint32_t> busErrors{0};
  std::atomic<std::uint32_t> stateChanges{0};
  // Transmitting: frames acknowledged, frames the controller gave up on, and frames that never
  // got as far as the controller (no free slot, bus-off).
  std::atomic<std::uint32_t> sent{0};
  std::atomic<std::uint32_t> sendFailed{0};
  std::atomic<std::uint32_t> sendRefused{0};
  std::atomic<std::uint32_t> recoveries{0};
};

struct CanBusStatus {
  const char* state;
  unsigned rxErrorCount;
};

class CanBus final : public obd::CanTransmitter {
 public:
  // Frames waiting to go out, or going: the driver holds on to each until it's sent.
  static constexpr std::size_t kTransmitSlots = 8;

  CanBus() = default;
  CanBus(const CanBus&) = delete;
  CanBus& operator=(const CanBus&) = delete;

  bool start(int txGpio, int rxGpio, bool transmit);
  bool receive(ReceivedFrame& frame, TickType_t timeout);
  CanBusStatus status() const;
  const CanBusCounters& counters() const { return counters_; }
  bool transmitting() const { return transmitting_; }

  // From the decode task only.
  std::optional<std::string> transmit(std::uint32_t id, const obd::Payload& data) override;
  // A node that went bus-off stays silent until told to recover; this does, at most once a
  // second.
  void recoverIfBusOff(std::int64_t nowMs);

 private:
  struct TransmitSlot {
    twai_frame_t frame = {};
    std::array<std::uint8_t, 8> data = {};
    std::atomic<bool> busy{false};
  };

  static bool onTransmitted(twai_node_handle_t node, const twai_tx_done_event_data_t* event, void* context);
  static bool onReceive(twai_node_handle_t node, const twai_rx_done_event_data_t* event, void* context);
  static bool onError(twai_node_handle_t node, const twai_error_event_data_t* event, void* context);
  static bool onStateChange(twai_node_handle_t node, const twai_state_change_event_data_t* event, void* context);

  twai_node_handle_t node_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  CanBusCounters counters_;
  bool transmitting_ = false;
  std::array<TransmitSlot, kTransmitSlots> slots_;
  std::atomic<bool> busOff_{false};
  std::int64_t lastRecoveryMs_ = -1000000;
};

}  // namespace emcool
