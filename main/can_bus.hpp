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
#include <mutex>
#include <optional>
#include <span>
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

  // The OBD side's fixed eight-byte request (from the diagnostics task).
  std::optional<std::string> transmit(std::uint32_t id, const obd::Payload& data) override;
  // A raw frame from the USB console's can-send: a standard id and 0-8 data bytes. Shares the
  // transmit slots with the OBD side, so it is serialised the same way.
  std::optional<std::string> sendRaw(std::uint32_t id, std::span<const std::uint8_t> data);
  // A node that went bus-off stays silent until told to recover; this does, at most once a
  // second.
  void recoverIfBusOff(std::int64_t nowMs);

  // The USB console's can-dump: every received frame is copied here while it is on, for the
  // console task to print. startDump() is cheap to call again (it empties what was waiting).
  bool startDump();
  void stopDump();
  bool nextDumpFrame(ReceivedFrame& frame, TickType_t timeout);
  std::uint32_t dumpDropped() const { return dumpDropped_.load(std::memory_order_relaxed); }

 private:
  // At most 8 bytes to the transmit slots, honouring the DLC; the shared slot picking is under
  // txMutex_ so the console and the diagnostics task never claim the same slot.
  std::optional<std::string> enqueue(std::uint32_t id, const std::uint8_t* data, std::size_t length);

  struct TransmitSlot {
    twai_frame_t frame = {};
    std::array<std::uint8_t, 8> data = {};
    std::atomic<bool> busy{false};
  };

  static bool onTransmitted(twai_node_handle_t node, const twai_tx_done_event_data_t* event, void* context);
  static bool onReceive(twai_node_handle_t node, const twai_rx_done_event_data_t* event, void* context);
  static bool onError(twai_node_handle_t node, const twai_error_event_data_t* event, void* context);
  static bool onStateChange(twai_node_handle_t node, const twai_state_change_event_data_t* event, void* context);

  // A short burst of received frames held for the console to print; a full one drops the newest.
  static constexpr UBaseType_t kDumpQueueDepth = 64;

  twai_node_handle_t node_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  CanBusCounters counters_;
  bool transmitting_ = false;
  std::array<TransmitSlot, kTransmitSlots> slots_;
  std::mutex txMutex_;
  std::atomic<bool> busOff_{false};
  std::int64_t lastRecoveryMs_ = -1000000;
  // The dump queue is made the first time a console asks and kept after, so can-dump needn't
  // allocate on a loaded heap each time.
  QueueHandle_t dumpQueue_ = nullptr;
  std::atomic<bool> dumping_{false};
  std::atomic<std::uint32_t> dumpDropped_{0};
};

// The bus the USB console's can-send and can-dump use; null until app_main sets it, and so left
// off the Wi-Fi and Bluetooth consoles, which never see these commands.
void setConsoleBus(CanBus* bus);
CanBus* consoleBus();

}  // namespace emcool
