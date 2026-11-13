#pragma once

// The decode side of the ride log: the SignalStore's sink. Readings that passed their
// deadband become `[ts, key, value, seq]` rows in the current segment, and a segment is
// sealed (handed to the writer) when the next row won't fit, when the clock's trust changes,
// when it is 5 s old, or when the bike parks, because parking is the only warning this bus
// gives before the power goes. Unencrypted, with segments sized for an ESP32's RAM.

#include <cstdint>
#include <optional>
#include <span>

#include "emcool/clock/system_clock_sync.hpp"
#include "emcool/ride_log/record.hpp"
#include "emcool/ride_log/segment.hpp"
#include "emcool/signals/signal_store.hpp"

namespace emcool::ride_log {

struct RideLogCounters {
  std::uint64_t readingsAppended = 0;
  // Both buffers were still with the writer, or a sealed segment could not be queued.
  std::uint64_t readingsDropped = 0;
  std::uint64_t segmentsSealed = 0;
  std::uint64_t segmentsLost = 0;
  std::uint64_t parkSeals = 0;
};

class RideLog final : public signals::RideLogSink {
 public:
  // RAM allows ~1 s of riding per segment, and this bounds how long a quiet, parked bike's
  // readings wait before reaching the card.
  static constexpr std::int64_t kMaxSegmentAgeMs = 5000;
  static constexpr std::int64_t kMinMsBetweenParkSeals = 5000;
  // V_VEHICLE_STATE = parked, 0x101 b1 (`vehicle_state_can`).
  static constexpr double kParkedVehicleState = 60;

  RideLog(const SessionId& session, std::span<SegmentBuffer> buffers, SegmentQueue& queue,
          const clock::ClockTrustSource& clockTrust);
  RideLog(const RideLog&) = delete;
  RideLog& operator=(const RideLog&) = delete;

  void appendReading(std::int64_t wallClockMs, std::int64_t monotonicMs, const signals::SignalDef& signal,
                     double value) override;

  // Seals a segment that has been open for kMaxSegmentAgeMs. Call it at least once a second.
  void tick(std::int64_t monotonicMs);
  void seal();

  const RideLogCounters& counters() const { return counters_; }

 private:
  bool beginSegment(SegmentBuffer& buffer, clock::ClockTrust trust, std::int64_t monotonicMs);
  SegmentBuffer* findFreeBuffer();
  void sealIfParking(const signals::SignalDef& signal, double value, std::int64_t monotonicMs);

  const SessionId session_;
  std::span<SegmentBuffer> buffers_;
  SegmentQueue& queue_;
  const clock::ClockTrustSource& clockTrust_;
  SegmentBuffer* active_ = nullptr;
  std::uint64_t nextSequence_ = 0;
  std::optional<double> lastVehicleState_;
  std::optional<std::int64_t> lastParkSealAtMs_;
  RideLogCounters counters_;
};

}  // namespace emcool::ride_log
