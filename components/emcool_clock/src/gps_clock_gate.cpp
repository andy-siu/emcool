#include "emcool/clock/gps_clock_gate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// The operation order follows the TypeScript reference, so the differential check in
// host/tests/check_clock.cpp can require identical verdicts.

namespace emcool::clock {

const char* verdictReasonName(ClockVerdictReason reason) {
  switch (reason) {
    case ClockVerdictReason::kColdBoot:
      return "cold-boot";
    case ClockVerdictReason::kDrift:
      return "drift";
    case ClockVerdictReason::kClockImplausible:
      return "clock-implausible";
    case ClockVerdictReason::kNotATime:
      return "not-a-time";
    case ClockVerdictReason::kBeforeFloor:
      return "before-floor";
    case ClockVerdictReason::kAwaitingCorroboration:
      return "awaiting-corroboration";
    case ClockVerdictReason::kInconsistentReadings:
      return "inconsistent-readings";
    case ClockVerdictReason::kDisagreesWithKnownGood:
      return "disagrees-with-known-good";
    case ClockVerdictReason::kInAgreement:
      return "in-agreement";
    case ClockVerdictReason::kCoolingDown:
      return "cooling-down";
  }
  return "unknown";
}

ClockVerdict GpsClockGate::offer(double epochSeconds, double systemEpochSeconds, double monotonicMs) {
  if (!std::isfinite(epochSeconds)) {
    return {false, ClockVerdictReason::kNotATime, 0, 0, 0};
  }
  if (epochSeconds < kGpsUtcFloorEpochSeconds) {
    return {false, ClockVerdictReason::kBeforeFloor, 0, 0, epochSeconds};
  }

  // Push, drop readings older than the window, keep at most the newest five — the reference's order.
  TimeReading candidates[kRequiredConsistentReadings + 1];
  std::size_t candidateCount = 0;
  for (std::size_t index = 0; index < windowCount_; index++) {
    candidates[candidateCount++] = window_[index];
  }
  candidates[candidateCount++] = {epochSeconds, monotonicMs};
  std::size_t keptCount = 0;
  for (std::size_t index = 0; index < candidateCount; index++) {
    if (monotonicMs - candidates[index].monotonicMs <= kReadingWindowMaxAgeMs) {
      candidates[keptCount++] = candidates[index];
    }
  }
  const std::size_t firstKept = keptCount > kRequiredConsistentReadings ? keptCount - kRequiredConsistentReadings : 0;
  windowCount_ = keptCount - firstKept;
  for (std::size_t index = 0; index < windowCount_; index++) {
    window_[index] = candidates[firstKept + index];
  }

  const std::uint32_t needed = anchorWasContested_ ? kReadingsToDisplaceContestedAnchor
                                                   : static_cast<std::uint32_t>(kRequiredConsistentReadings);
  const double disagreement = worstDisagreementSeconds(epochSeconds, monotonicMs);
  if (disagreement > kConsistencyToleranceSeconds) {
    consecutiveConsistent_ = 1;
    return {false, ClockVerdictReason::kInconsistentReadings, 0, 0, disagreement};
  }
  consecutiveConsistent_++;
  if (windowCount_ < kRequiredConsistentReadings || consecutiveConsistent_ < needed) {
    return {false, ClockVerdictReason::kAwaitingCorroboration, 0, 0,
            static_cast<double>(std::min(consecutiveConsistent_, needed))};
  }

  std::optional<TimeReading> anchor;
  if (knownGood_ && monotonicMs - knownGood_->monotonicMs <= kKnownGoodMaxAgeMs) {
    anchor = knownGood_;
  }
  if (anchor) {
    const double projected = anchor->epochSeconds + (monotonicMs - anchor->monotonicMs) / 1000;
    const double driftFromAnchor = std::fabs(epochSeconds - projected);
    if (driftFromAnchor > kKnownGoodToleranceSeconds) {
      anchorWasContested_ = true;
      return {false, ClockVerdictReason::kDisagreesWithKnownGood, 0, 0, driftFromAnchor};
    }
  }

  const bool wasColdBoot = !anchor.has_value();
  knownGood_ = TimeReading{epochSeconds, monotonicMs};
  anchorWasContested_ = false;
  const double offsetSeconds = epochSeconds - systemEpochSeconds;
  if (std::fabs(offsetSeconds) <= kDriftThresholdSeconds) {
    return {false, ClockVerdictReason::kInAgreement, 0, 0, offsetSeconds};
  }

  const double sinceLastStepMs =
      lastStepAtMonotonicMs_ ? monotonicMs - *lastStepAtMonotonicMs_ : std::numeric_limits<double>::infinity();
  const bool clockIsImplausible =
      systemEpochSeconds < kGpsUtcFloorEpochSeconds || std::fabs(offsetSeconds) >= kCooldownOverrideSeconds;
  if (sinceLastStepMs < kMinSecondsBetweenSteps * 1000 && !clockIsImplausible) {
    return {false, ClockVerdictReason::kCoolingDown, 0, 0, offsetSeconds};
  }
  lastStepAtMonotonicMs_ = monotonicMs;
  const ClockVerdictReason reason = wasColdBoot          ? ClockVerdictReason::kColdBoot
                                    : clockIsImplausible ? ClockVerdictReason::kClockImplausible
                                                         : ClockVerdictReason::kDrift;
  return {true, reason, epochSeconds, offsetSeconds, offsetSeconds};
}

double GpsClockGate::worstDisagreementSeconds(double epochSeconds, double monotonicMs) const {
  double worst = 0;
  for (std::size_t index = 0; index < windowCount_; index++) {
    const double projected = window_[index].epochSeconds + (monotonicMs - window_[index].monotonicMs) / 1000;
    worst = std::max(worst, std::fabs(projected - epochSeconds));
  }
  return worst;
}

}  // namespace emcool::clock
