#include "emcool/vcu/charge_auto_curve.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace emcool::vcu {

namespace {

constexpr double kFullRequestA = 75;
constexpr double kTaperKneeSoc = 88;
constexpr double kSocRateTrustedBelow = 88;

// JavaScript Math.round: round half UP (toward +infinity), not to-even.
double jsRoundHalfUp(double value) { return std::floor(value + 0.5); }

const TemperatureSample* newestSampleAtOrBefore(const std::vector<TemperatureSample>& samples, std::int64_t atMs) {
  const TemperatureSample* found = nullptr;
  for (const TemperatureSample& sample : samples) {
    if (sample.atMs <= atMs) found = &sample;  // findLast: the last in order satisfying the predicate
  }
  return found;
}

std::optional<double> minutesSinceNewestSample(const std::vector<TemperatureSample>& samples, std::int64_t nowMs) {
  const TemperatureSample* newest = newestSampleAtOrBefore(samples, nowMs);
  if (newest == nullptr) return std::nullopt;
  return static_cast<double>(nowMs - newest->atMs) / 60000.0;
}

double leastSquaresSlopePerMinute(const std::vector<TemperatureSample>& window) {
  double meanAt = 0;
  double meanCelsius = 0;
  for (const TemperatureSample& sample : window) {
    meanAt += static_cast<double>(sample.atMs);
    meanCelsius += sample.celsius;
  }
  meanAt /= static_cast<double>(window.size());
  meanCelsius /= static_cast<double>(window.size());
  double covariance = 0;
  double variance = 0;
  for (const TemperatureSample& sample : window) {
    covariance += (static_cast<double>(sample.atMs) - meanAt) * (sample.celsius - meanCelsius);
    // cool-eva writes `(sample.atMs - meanAt) ** 2`; std::pow(.,2) matches V8's ** last bit.
    variance += std::pow(static_cast<double>(sample.atMs) - meanAt, 2);
  }
  return variance == 0 ? 0 : (covariance / variance) * 60000.0;
}

double taperEnvelopeAmpsAt(int socPercent) {
  if (socPercent < kTaperKneeSoc) return kFullRequestA;
  switch (socPercent) {
    case 88: return 70;
    case 89: return 65;
    case 90: return 62;
    case 91: return 60;
    case 92: return 58;
    case 93: return 53;
    case 94: return 49;
    case 95: return 45;
    case 96: return 41;
    case 97: return 37;
    case 98: return 32;
    case 99: return 29;
    case 100: return 5;
    default: return kFullRequestA;
  }
}

std::optional<int> socWhereTaperFallsBelow(double amps) {
  for (int soc = static_cast<int>(kTaperKneeSoc); soc <= 100; soc++) {
    if (taperEnvelopeAmpsAt(soc) < amps) return soc;
  }
  return std::nullopt;
}

std::optional<double> minutesUntilTaperBites(double socPercent, double requestedAmps, double percentPerMinute) {
  const std::optional<int> bitesAt = socWhereTaperFallsBelow(requestedAmps);
  if (!bitesAt || static_cast<double>(*bitesAt) <= socPercent) return std::nullopt;
  return (static_cast<double>(*bitesAt) - socPercent) / percentPerMinute;
}

double confidentHeadroomKelvin(const std::vector<TemperatureSample>& samples, std::int64_t nowMs,
                               double headroomKelvin) {
  const std::optional<double> silentMinutes = minutesSinceNewestSample(samples, nowMs);
  return !silentMinutes ? 0 : headroomKelvin - kQuantisationK - kReactionMin / *silentMinutes;
}

double clampedStep(double kelvin) {
  return std::min(kMaxStepA, std::max(kMinStepA, jsRoundHalfUp(kAmpsPerKelvin * kelvin)));
}

double blindStepAmps(const std::vector<TemperatureSample>& samples, std::int64_t nowMs) {
  const std::optional<double> silentMinutes = minutesSinceNewestSample(samples, nowMs);
  return !silentMinutes ? kMaxStepA : clampedStep(kReactionMin / *silentMinutes);
}

bool readingRoseSince(const std::vector<TemperatureSample>& samples, std::int64_t nowMs,
                      const TemperatureSample* atCommand) {
  if (atCommand == nullptr) return false;
  for (const TemperatureSample& sample : samples) {
    if (sample.atMs > atCommand->atMs && sample.atMs <= nowMs && sample.celsius > atCommand->celsius) return true;
  }
  return false;
}

bool cooledFromBelowSetpoint(const std::vector<TemperatureSample>& samples, std::int64_t nowMs, double setpointC,
                             const TemperatureSample* atCommand) {
  const TemperatureSample* newest = newestSampleAtOrBefore(samples, nowMs);
  if (atCommand == nullptr || newest == nullptr) return true;
  if (atCommand->celsius >= setpointC) return false;
  return newest->celsius < atCommand->celsius;
}

std::int64_t measurableAfterMs(const std::vector<TemperatureSample>& samples, std::int64_t nowMs, bool rateIsNotRising,
                               const TemperatureSample* atCommand) {
  if (rateIsNotRising && readingRoseSince(samples, nowMs, atCommand)) return kRateWindowMs;
  return kRateMinSpanMs;
}

bool commandIsUnmeasured(const std::vector<TemperatureSample>& samples, std::int64_t nowMs,
                         std::optional<std::int64_t> lastCommandAtMs, bool rateIsNotRising, double setpointC) {
  if (!lastCommandAtMs) return false;
  const TemperatureSample* atCommand = newestSampleAtOrBefore(samples, *lastCommandAtMs);
  if (cooledFromBelowSetpoint(samples, nowMs, setpointC, atCommand)) return false;
  return nowMs - *lastCommandAtMs < measurableAfterMs(samples, nowMs, rateIsNotRising, atCommand);
}

double predictedHeadroomKelvin(double temperatureC, double ratePerMinute, double horizonMinutes) {
  return kTargetC - temperatureC - ratePerMinute * horizonMinutes;
}

ChargeAutoDecision hold(ChargeAutoReason reason) { return {ChargeAutoDecision::Kind::kHold, 0, reason}; }

ChargeAutoDecision stepTo(double wanted, int current, double ceiling, ChargeAutoReason reason) {
  if (ceiling <= kMinCommandA) return hold(ChargeAutoReason::kAtFloor);
  const int amps = static_cast<int>(std::min(std::floor(ceiling), std::max(static_cast<double>(kMinCommandA), jsRoundHalfUp(wanted))));
  if (amps == current) {
    return hold(amps == kMinCommandA ? ChargeAutoReason::kAtFloor : ChargeAutoReason::kSettled);
  }
  return {ChargeAutoDecision::Kind::kCommand, amps, reason};
}

// forward
std::optional<ChargeAutoDecision> measurementPending(const ChargeAutoInput& input, const ChargeAutoDecision& stepped,
                                                     int current, double temperature, const HeatingRate& rate);
std::optional<ChargeAutoDecision> sessionEndsFirst(const ChargeAutoInput& input, const ChargeAutoDecision& stepped,
                                                   int current, double temperature, const HeatingRate& rate);

}  // namespace

bool isPackTemperaturePlausible(std::optional<double> celsius) {
  return celsius && std::isfinite(*celsius) && *celsius >= kPackTemperatureMinC && *celsius <= kPackTemperatureMaxC;
}

bool isSocPlausible(std::optional<double> percent) {
  return percent && std::isfinite(*percent) && *percent >= 0 && *percent <= 100;
}

HeatingRate estimateHeatingRate(const std::vector<TemperatureSample>& samples, std::int64_t nowMs) {
  const std::int64_t from = nowMs - kRateWindowMs;
  std::vector<TemperatureSample> inWindow;
  for (const TemperatureSample& sample : samples) {
    if (sample.atMs >= from && sample.atMs <= nowMs) inWindow.push_back(sample);
  }
  const TemperatureSample* anchor = nullptr;
  for (const TemperatureSample& sample : samples) {
    if (sample.atMs < from) anchor = &sample;  // .at(-1) of the before-window samples
  }
  std::vector<TemperatureSample> window;
  if (anchor != nullptr) window.push_back(*anchor);
  window.insert(window.end(), inWindow.begin(), inWindow.end());
  if (window.empty()) return {HeatingRate::Kind::kUnknown, 0};
  const std::int64_t spanMs = std::min(nowMs - window.front().atMs, kRateWindowMs);
  if (spanMs < kRateMinSpanMs) return {HeatingRate::Kind::kUnknown, 0};
  std::set<double> distinctSet;
  for (const TemperatureSample& sample : window) distinctSet.insert(sample.celsius);
  const int distinct = static_cast<int>(distinctSet.size());
  if (distinct < kRateMinDistinct) {
    if (window.back().celsius <= window.front().celsius) return {HeatingRate::Kind::kNotRising, 0};
    return {HeatingRate::Kind::kBounded, (distinct * 60000.0) / static_cast<double>(spanMs)};
  }
  const double slope = leastSquaresSlopePerMinute(window);
  const std::optional<double> silentMinutes = minutesSinceNewestSample(window, nowMs);
  if (!silentMinutes) return {HeatingRate::Kind::kRate, slope};
  const double bound = 1.0 / *silentMinutes;
  return {HeatingRate::Kind::kRate, std::max(-bound, std::min(slope, bound))};
}

double measuredRatePerMinute(const HeatingRate& rate) {
  return (rate.kind == HeatingRate::Kind::kRate || rate.kind == HeatingRate::Kind::kBounded) ? rate.perMinute : 0;
}

std::optional<double> estimateSocRate(const std::vector<SocSample>& samples, std::int64_t nowMs) {
  const std::int64_t from = nowMs - kSocWindowMs;
  std::vector<SocSample> window;
  for (const SocSample& sample : samples) {
    if (sample.atMs >= from && sample.atMs <= nowMs) window.push_back(sample);
  }
  if (window.size() < 2) return std::nullopt;
  const std::int64_t spanMs = nowMs - window.front().atMs;
  if (spanMs < kRateMinSpanMs) return std::nullopt;
  std::set<double> distinctSet;
  for (const SocSample& sample : window) distinctSet.insert(sample.percent);
  if (static_cast<int>(distinctSet.size()) < kRateMinDistinct) return std::nullopt;
  const double advanced = window.back().percent - window.front().percent;
  if (advanced <= 0) return std::nullopt;
  return advanced / (static_cast<double>(spanMs) / 60000.0);
}

std::optional<double> sessionAheadMinutes(std::optional<double> socPercent, std::optional<double> socAgeMs,
                                          const std::vector<SocSample>& socSamples, std::optional<double> requestedAmps,
                                          std::int64_t nowMs) {
  if (!isSocPlausible(socPercent) || !socAgeMs || *socAgeMs > kSocMaxAgeMs) return std::nullopt;
  if (*socPercent >= kSocRateTrustedBelow) return std::nullopt;
  const std::optional<double> percentPerMinute = estimateSocRate(socSamples, nowMs);
  if (!percentPerMinute) return std::nullopt;
  if (!requestedAmps) return std::nullopt;
  return minutesUntilTaperBites(*socPercent, *requestedAmps, *percentPerMinute);
}

ChargeAutoDecision decideChargeCurrent(const ChargeAutoInput& input) {
  if (!input.enabled) return hold(ChargeAutoReason::kDisabled);
  if (input.riderOverride) return hold(ChargeAutoReason::kRider);
  if (input.chargeManagerState != kAutoChargeManagerStateDc || !input.chargeManagerStateAgeMs ||
      *input.chargeManagerStateAgeMs > kChargeAutoSessionMaxAgeMs) {
    return hold(ChargeAutoReason::kNotDc);
  }
  if (!input.packTemperatureC || !input.packTemperaturePlausible || !input.packTemperatureAgeMs ||
      *input.packTemperatureAgeMs > kTemperatureMaxAgeMs) {
    return hold(ChargeAutoReason::kNoTemperature);
  }
  if (!input.ceilingAmps) return hold(ChargeAutoReason::kNoCeiling);

  const double temperature = *input.packTemperatureC;
  const double ceiling = *input.ceilingAmps;
  const int current = static_cast<int>(std::floor(input.commandedAmps.value_or(ceiling)));
  const HeatingRate rate = estimateHeatingRate(input.samples, input.nowMs);

  if (temperature >= kCliffC) {
    return stepTo(current - kMaxStepA, current, ceiling, ChargeAutoReason::kHardCeiling);
  }
  if (rate.kind == HeatingRate::Kind::kUnknown) {
    if (temperature < kTargetC) return hold(ChargeAutoReason::kNoHistory);
    return stepTo(current - blindStepAmps(input.samples, input.nowMs), current, ceiling,
                  ChargeAutoReason::kBlindDescent);
  }
  if (temperature >= kTargetC &&
      (rate.kind == HeatingRate::Kind::kBounded || rate.kind == HeatingRate::Kind::kNotRising)) {
    return hold(ChargeAutoReason::kNearCeiling);
  }
  const double headroomKelvin = predictedHeadroomKelvin(temperature, measuredRatePerMinute(rate), kReactionMin);
  if (temperature >= kTargetC && headroomKelvin >= 0) return hold(ChargeAutoReason::kNearCeiling);
  if (temperature < kTargetC && std::fabs(headroomKelvin) < kQuantisationK) return hold(ChargeAutoReason::kSettled);

  const double spendableKelvin = rate.kind == HeatingRate::Kind::kNotRising
                                     ? confidentHeadroomKelvin(input.samples, input.nowMs, headroomKelvin)
                                     : std::fabs(headroomKelvin);
  if (rate.kind == HeatingRate::Kind::kNotRising && spendableKelvin <= 0) return hold(ChargeAutoReason::kSettled);

  const double step = clampedStep(spendableKelvin);
  if (headroomKelvin > 0) {
    const ChargeAutoDecision raised = stepTo(current + step, current, ceiling, ChargeAutoReason::kClear);
    const std::optional<ChargeAutoDecision> pending = measurementPending(input, raised, current, temperature, rate);
    return pending ? *pending : raised;
  }
  const ChargeAutoReason lowering =
      temperature >= kTargetC ? ChargeAutoReason::kHardCeiling : ChargeAutoReason::kClosing;
  const ChargeAutoDecision stepped = stepTo(current - step, current, ceiling, lowering);
  const std::optional<ChargeAutoDecision> ends = sessionEndsFirst(input, stepped, current, temperature, rate);
  return ends ? *ends : stepped;
}

namespace {

std::optional<ChargeAutoDecision> measurementPending(const ChargeAutoInput& input, const ChargeAutoDecision& stepped,
                                                     int current, double temperature, const HeatingRate& rate) {
  if (temperature >= kTargetC || stepped.kind != ChargeAutoDecision::Kind::kCommand || stepped.amps <= current) {
    return std::nullopt;
  }
  if (current < kMinCommandA) return std::nullopt;
  const bool unmeasured = commandIsUnmeasured(input.samples, input.nowMs, input.lastCommandAtMs,
                                              rate.kind == HeatingRate::Kind::kNotRising, kTargetC);
  return unmeasured ? std::optional<ChargeAutoDecision>(hold(ChargeAutoReason::kMeasuring)) : std::nullopt;
}

std::optional<ChargeAutoDecision> sessionEndsFirst(const ChargeAutoInput& input, const ChargeAutoDecision& stepped,
                                                   int current, double temperature, const HeatingRate& rate) {
  if (temperature >= kTargetC || rate.kind != HeatingRate::Kind::kRate) return std::nullopt;
  if (input.requestedAmps && *input.requestedAmps <= kMinCommandA) return std::nullopt;
  if (stepped.kind != ChargeAutoDecision::Kind::kCommand || stepped.amps >= current) return std::nullopt;
  const std::optional<double> ahead =
      sessionAheadMinutes(input.socPercent, input.socAgeMs, input.socSamples, input.requestedAmps, input.nowMs);
  if (!ahead) return std::nullopt;
  if (predictedHeadroomKelvin(temperature, rate.perMinute, *ahead) < 0) return std::nullopt;
  return hold(ChargeAutoReason::kTapering);
}

}  // namespace

}  // namespace emcool::vcu
