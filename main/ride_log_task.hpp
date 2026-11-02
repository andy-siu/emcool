#pragma once

// The ride log's writer task: owns the microSD card and every write to it, so a slow card
// (a write can stall for hundreds of milliseconds) never holds up the decode task. Sealed
// segments arrive through a FreeRTOS queue; between them the task zero-fills ahead.

#include <atomic>
#include <cstdint>

#include "emcool/ble_server/endpoints.hpp"
#include "emcool/ride_log/log_writer.hpp"
#include "emcool/ride_log/segment.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sd_card.hpp"

namespace emcool {

class FreeRtosSegmentQueue final : public ride_log::SegmentQueue {
 public:
  bool create(UBaseType_t depth);
  bool submit(ride_log::SegmentBuffer& buffer) override;
  bool receive(ride_log::SegmentBuffer*& buffer, TickType_t timeout);

 private:
  QueueHandle_t queue_ = nullptr;
};

struct RideLogTaskCounters {
  // Sealed segments that arrived with no card to write them to, or failed to write.
  std::atomic<std::uint32_t> segmentsLost{0};
  std::atomic<std::uint32_t> readingsLost{0};
  std::atomic<bool> cardMounted{false};
};

struct RideLogTaskContext {
  FreeRtosSegmentQueue& queue;
  ride_log::LogWriter& writer;
  SdCard& card;
  RideLogTaskCounters& counters;
};

bool startRideLogTask(RideLogTaskContext& context);

// The ride log as /status reports it: parts on the card, at their allocated size (the part
// being written counts in full), and the card's space.
class RideLogStorageReport final : public ble_server::StorageReport {
 public:
  RideLogStorageReport(const ride_log::LogWriter& writer, const RideLogTaskCounters& counters)
      : writer_(writer), counters_(counters) {}
  ble_server::StorageStatus storage() override;

 private:
  const ride_log::LogWriter& writer_;
  const RideLogTaskCounters& counters_;
};

}  // namespace emcool
