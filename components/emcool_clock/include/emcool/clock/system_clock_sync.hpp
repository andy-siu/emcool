#pragma once

// Whether the wall clock can be believed, and when to step it to GPS time. A port of the
// state in cool-eva src/gps/clock.ts with the `date -u -s` call taken out: offer() says when
// to step, the caller steps (settimeofday on the ESP32, nothing on the host) and reports
// back. The ESP32 has no battery-backed clock, so every boot starts "never-synced".

#include <cstdint>
#include <optional>

#include "emcool/clock/gps_clock_gate.hpp"

namespace emcool::clock {

// cool-eva's ClockTrust, sealed into every ride-log segment so the laptop can tell which
// timestamps to doubt. `seq` orders readings whatever this says.
enum class ClockTrust : std::uint8_t { kSatelliteBacked, kNeverSynced, kContested };

const char* clockTrustName(ClockTrust trust);

class ClockTrustSource {
 public:
  virtual ~ClockTrustSource() = default;
  virtual ClockTrust trust() const = 0;
};

class SystemClockSync final : public ClockTrustSource {
 public:
  // The step verdict — target epoch seconds, offset, reason — if the clock should be stepped now.
  std::optional<ClockVerdict> offer(double gpsEpochSeconds, double systemEpochSeconds, double monotonicMs);
  void noteStepResult(bool succeeded, double offsetSeconds, ClockVerdictReason reason);

  ClockTrust trust() const override;

 private:
  void reportHold(const ClockVerdict& verdict, double monotonicMs);

  GpsClockGate gate_;
  bool confirmed_ = false;
  bool contested_ = false;
  std::uint32_t failedSteps_ = 0;
  std::optional<ClockVerdictReason> lastHoldReason_;
  double lastHoldReasonAtMs_ = 0;
};

}  // namespace emcool::clock
