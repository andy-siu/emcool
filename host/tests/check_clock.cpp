// Replays the offers written by tools/generate-golden-clock.mts — cool-eva's GpsClockGate
// driven through randomized clock scenarios — through the C++ gate, and requires the same
// verdict every time: step or hold, the reason, and for a step the exact target and offset.
// Then checks SystemClockSync's trust transitions against cool-eva's clock.ts rules.
//
//   check_clock <golden clock file>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "check.hpp"
#include "emcool/clock/gps_clock_gate.hpp"
#include "emcool/clock/system_clock_sync.hpp"

namespace {

bool sameNumber(double expected, double actual) {
  if (std::isnan(expected) || std::isnan(actual)) {
    return std::isnan(expected) && std::isnan(actual);
  }
  return expected == actual;
}

void checkTrustTransitions();

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_clock <golden file from tools/generate-golden-clock.mts>\n");
    return 2;
  }
  std::ifstream golden(arguments[1]);
  if (!golden) {
    std::fprintf(stderr, "check_clock: cannot open %s\n", arguments[1]);
    return 2;
  }

  std::unique_ptr<emcool::clock::GpsClockGate> gate;
  long currentRun = -1;
  std::string line;
  unsigned long long offers = 0;
  unsigned long long steps = 0;
  int reported = 0;
  while (std::getline(golden, line)) {
    std::istringstream fields(line);
    long run = 0;
    std::string gpsText, systemText, monotonicText, reasonText;
    int step = 0;
    if (!(fields >> run >> gpsText >> systemText >> monotonicText >> step >> reasonText)) {
      std::fprintf(stderr, "check_clock: unparseable line: %s\n", line.c_str());
      return 2;
    }
    if (run != currentRun) {
      gate = std::make_unique<emcool::clock::GpsClockGate>();
      currentRun = run;
    }
    const double gpsEpoch = std::strtod(gpsText.c_str(), nullptr);
    const double systemEpoch = std::strtod(systemText.c_str(), nullptr);
    const double monotonicMs = std::strtod(monotonicText.c_str(), nullptr);
    const emcool::clock::ClockVerdict verdict = gate->offer(gpsEpoch, systemEpoch, monotonicMs);
    offers++;

    bool matches = verdict.step == (step == 1) && reasonText == emcool::clock::verdictReasonName(verdict.reason);
    if (matches && step == 1) {
      steps++;
      std::string epochText, offsetText;
      fields >> epochText >> offsetText;
      matches = sameNumber(std::strtod(epochText.c_str(), nullptr), verdict.epochSeconds) &&
                sameNumber(std::strtod(offsetText.c_str(), nullptr), verdict.offsetSeconds);
    }
    if (!matches) {
      emcool::test::failures()++;
      if (reported < 20) {
        reported++;
        std::fprintf(stderr, "MISMATCH run %ld\n  golden: %s\n  c++   : step=%d %s %.17g %.17g\n", run, line.c_str(),
                     verdict.step ? 1 : 0, emcool::clock::verdictReasonName(verdict.reason), verdict.epochSeconds,
                     verdict.offsetSeconds);
      }
    }
  }

  checkTrustTransitions();
  std::printf("check_clock: %llu offers, %llu steps, %d failures\n", offers, steps, emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}

namespace {

// cool-eva clock.ts: never-synced until a step succeeds or the clock is found in agreement;
// contested after a failed step or a corroborated time that disagrees with the known-good one.
void checkTrustTransitions() {
  using emcool::clock::ClockTrust;
  emcool::clock::SystemClockSync sync;
  EMCOOL_CHECK(sync.trust() == ClockTrust::kNeverSynced, "a fresh clock is %s",
               emcool::clock::clockTrustName(sync.trust()));

  const double floor = emcool::clock::kGpsUtcFloorEpochSeconds;
  const double trueEpoch = floor + 86400 * 100;
  double monotonicMs = 1000;
  std::optional<emcool::clock::ClockVerdict> stepTo;
  // Five consistent readings against a 1970 clock: the fifth asks for a cold-boot step.
  for (int reading = 0; reading < 5; reading++) {
    stepTo = sync.offer(trueEpoch + reading, monotonicMs / 1000, monotonicMs);
    monotonicMs += 1000;
  }
  EMCOOL_CHECK(stepTo.has_value(), "five consistent readings did not ask for a step");
  EMCOOL_CHECK(sync.trust() == ClockTrust::kNeverSynced, "trust moved before the step was reported");
  sync.noteStepResult(false, 1.0, emcool::clock::ClockVerdictReason::kColdBoot);
  EMCOOL_CHECK(sync.trust() == ClockTrust::kContested, "a failed step left trust %s",
               emcool::clock::clockTrustName(sync.trust()));
  sync.noteStepResult(true, 1.0, emcool::clock::ClockVerdictReason::kColdBoot);
  EMCOOL_CHECK(sync.trust() == ClockTrust::kSatelliteBacked, "a successful step left trust %s",
               emcool::clock::clockTrustName(sync.trust()));
}

}  // namespace
