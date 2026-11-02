#include "emcool/clock/system_clock_sync.hpp"

#include "emcool/log.hpp"

namespace emcool::clock {

namespace {

constexpr const char* kTag = "clock";
// A hold whose reason hasn't changed is repeated at most this often, so a clock stuck
// waiting for corroboration says so without filling the console.
constexpr double kRepeatHoldReasonAfterMs = 300000;

bool isAlarming(ClockVerdictReason reason) {
  return reason == ClockVerdictReason::kNotATime || reason == ClockVerdictReason::kBeforeFloor ||
         reason == ClockVerdictReason::kInconsistentReadings ||
         reason == ClockVerdictReason::kDisagreesWithKnownGood;
}

}  // namespace

const char* clockTrustName(ClockTrust trust) {
  switch (trust) {
    case ClockTrust::kSatelliteBacked:
      return "satellite-backed";
    case ClockTrust::kNeverSynced:
      return "never-synced";
    case ClockTrust::kContested:
      return "contested";
  }
  return "contested";
}

std::optional<ClockVerdict> SystemClockSync::offer(double gpsEpochSeconds, double systemEpochSeconds,
                                                   double monotonicMs) {
  const ClockVerdict verdict = gate_.offer(gpsEpochSeconds, systemEpochSeconds, monotonicMs);
  if (verdict.step) {
    return verdict;
  }
  if (verdict.reason == ClockVerdictReason::kInAgreement) {
    confirmed_ = true;
    contested_ = false;
  }
  if (verdict.reason == ClockVerdictReason::kDisagreesWithKnownGood) {
    contested_ = true;
  }
  reportHold(verdict, monotonicMs);
  return std::nullopt;
}

void SystemClockSync::noteStepResult(bool succeeded, double offsetSeconds, ClockVerdictReason reason) {
  if (succeeded) {
    confirmed_ = true;
    contested_ = false;
    logWarning(kTag, "system time was %.1f s off GPS (%s) — stepped", offsetSeconds, verdictReasonName(reason));
    return;
  }
  contested_ = true;
  failedSteps_++;
  if (failedSteps_ == 1 || failedSteps_ % 100 == 0) {
    logWarning(kTag, "failed to set system time from GPS (attempt %u)", static_cast<unsigned>(failedSteps_));
  }
}

ClockTrust SystemClockSync::trust() const {
  if (contested_) {
    return ClockTrust::kContested;
  }
  return confirmed_ ? ClockTrust::kSatelliteBacked : ClockTrust::kNeverSynced;
}

void SystemClockSync::reportHold(const ClockVerdict& verdict, double monotonicMs) {
  const bool changed = !lastHoldReason_ || *lastHoldReason_ != verdict.reason;
  if (!changed && monotonicMs - lastHoldReasonAtMs_ < kRepeatHoldReasonAfterMs) {
    return;
  }
  lastHoldReason_ = verdict.reason;
  lastHoldReasonAtMs_ = monotonicMs;
  if (isAlarming(verdict.reason)) {
    logWarning(kTag, "%s (%.3f)%s", verdictReasonName(verdict.reason), verdict.detail,
               changed ? "" : " — unchanged for 5 min");
    return;
  }
  logInfo(kTag, "%s (%.3f)%s", verdictReasonName(verdict.reason), verdict.detail,
          changed ? "" : " — unchanged for 5 min");
}

}  // namespace emcool::clock
