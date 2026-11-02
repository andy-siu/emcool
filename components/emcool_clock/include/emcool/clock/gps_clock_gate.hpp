#pragma once

// Decides whether a GPS time is good enough to step the system clock to. A port of cool-eva
// src/gps/clock-gate.ts: a time must be past the 2026 floor, corroborated by five readings
// that agree with each other within 3 s, and must agree within 10 s with a recent known-good
// time carried forward. Only then is a clock more than 60 s off stepped. The reasoning for
// each threshold is in that file.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace emcool::clock {

inline constexpr double kDriftThresholdSeconds = 60;
inline constexpr double kMinSecondsBetweenSteps = 300;
inline constexpr double kCooldownOverrideSeconds = 3600;
inline constexpr std::size_t kRequiredConsistentReadings = 5;
inline constexpr double kConsistencyToleranceSeconds = 3;
inline constexpr double kReadingWindowMaxAgeMs = 30000;
inline constexpr double kKnownGoodMaxAgeMs = 600000;
inline constexpr std::uint32_t kReadingsToDisplaceContestedAnchor = 30;
inline constexpr double kKnownGoodToleranceSeconds = 10;
// 2026-01-01T00:00:00Z, the same floor the GPS decoder applies.
inline constexpr double kGpsUtcFloorEpochSeconds = 1767225600;

enum class ClockVerdictReason : std::uint8_t {
  // Steps.
  kColdBoot,
  kDrift,
  kClockImplausible,
  // Holds.
  kNotATime,
  kBeforeFloor,
  kAwaitingCorroboration,
  kInconsistentReadings,
  kDisagreesWithKnownGood,
  kInAgreement,
  kCoolingDown,
};

// The cool-eva spelling, which tests compare against and logs print.
const char* verdictReasonName(ClockVerdictReason reason);

struct ClockVerdict {
  bool step;
  ClockVerdictReason reason;
  // Set when `step`: the time to step to, and how far the system clock was from it.
  double epochSeconds;
  double offsetSeconds;
  // For the log line of a hold: the measured quantity the reason is about (seconds of
  // disagreement or offset, or the corroboration count) — 0 where there is none.
  double detail;
};

class GpsClockGate {
 public:
  ClockVerdict offer(double epochSeconds, double systemEpochSeconds, double monotonicMs);

 private:
  struct TimeReading {
    double epochSeconds;
    double monotonicMs;
  };

  double worstDisagreementSeconds(double epochSeconds, double monotonicMs) const;

  TimeReading window_[kRequiredConsistentReadings] = {};
  std::size_t windowCount_ = 0;
  std::optional<TimeReading> knownGood_;
  std::optional<double> lastStepAtMonotonicMs_;
  std::uint32_t consecutiveConsistent_ = 0;
  bool anchorWasContested_ = false;
};

}  // namespace emcool::clock
