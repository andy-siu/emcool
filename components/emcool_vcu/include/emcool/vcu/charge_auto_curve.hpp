#pragma once

// The automatic charge-current decision and its support. Pure: the pack-temperature and SoC sample
// histories, the live readings and now, in; a command-or-hold decision out. The control law aims
// the pack at TARGET_C (54), never past CLIFF_C (55), predicting heat over a REACTION_MIN window and
// stepping the current in clamped amps.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emcool::vcu {

// The control law's constants.
inline constexpr double kCliffC = 55;
inline constexpr double kTargetC = 54;
inline constexpr double kReactionMin = 12;
inline constexpr int kMinCommandA = 35;
inline constexpr double kAmpsPerKelvin = 2;
inline constexpr double kMinStepA = 1;
inline constexpr double kMaxStepA = 15;
inline constexpr double kQuantisationK = 0.5;
inline constexpr std::int64_t kTemperatureMaxAgeMs = 5000;
inline constexpr double kAutoChargeManagerStateDc = 0x23;
inline constexpr std::int64_t kChargeAutoSessionMaxAgeMs = 5000;
// The heating-rate estimate.
inline constexpr std::int64_t kRateMinSpanMs = 300000;
inline constexpr int kRateMinDistinct = 3;
inline constexpr std::int64_t kRateWindowMs = 600000;
// Pack temperature plausibility.
inline constexpr double kPackTemperatureMinC = -30;
inline constexpr double kPackTemperatureMaxC = 90;
// The SoC estimate.
inline constexpr std::int64_t kSocMaxAgeMs = 5000;
inline constexpr std::int64_t kSocWindowMs = kRateWindowMs;

enum class ChargeAutoReason : std::uint8_t {
  kDisabled = 0,
  kNotDc = 1,
  kNoTemperature = 2,
  kNoCeiling = 3,
  kRider = 4,
  kNoHistory = 5,
  kBlindDescent = 6,
  kHardCeiling = 7,
  kClosing = 8,
  kClear = 9,
  kSettled = 10,
  kAtFloor = 11,
  kNearCeiling = 12,
  kTapering = 13,
  kMeasuring = 14,
};

struct TemperatureSample {
  std::int64_t atMs;
  double celsius;
};
struct SocSample {
  std::int64_t atMs;
  double percent;
};

struct ChargeAutoDecision {
  enum class Kind : std::uint8_t { kCommand, kHold };
  Kind kind = Kind::kHold;
  int amps = 0;  // kCommand
  ChargeAutoReason reason = ChargeAutoReason::kDisabled;
};

struct ChargeAutoInput {
  bool enabled = false;
  std::optional<double> packTemperatureC;
  std::optional<double> packTemperatureAgeMs;
  bool packTemperaturePlausible = false;
  std::optional<double> chargeManagerState;
  std::optional<double> chargeManagerStateAgeMs;
  std::optional<double> ceilingAmps;
  std::optional<double> commandedAmps;
  bool riderOverride = false;
  std::vector<TemperatureSample> samples;
  std::optional<double> socPercent;
  std::optional<double> socAgeMs;
  std::vector<SocSample> socSamples;
  std::optional<double> requestedAmps;
  std::optional<std::int64_t> lastCommandAtMs;
  std::int64_t nowMs = 0;
};

ChargeAutoDecision decideChargeCurrent(const ChargeAutoInput& input);

bool isPackTemperaturePlausible(std::optional<double> celsius);
bool isSocPlausible(std::optional<double> percent);

// --- The heating rate, exported for the golden ------------------------------------------------
struct HeatingRate {
  enum class Kind : std::uint8_t { kRate, kBounded, kNotRising, kUnknown };
  Kind kind = Kind::kUnknown;
  double perMinute = 0;  // kRate, kBounded
};
HeatingRate estimateHeatingRate(const std::vector<TemperatureSample>& samples, std::int64_t nowMs);
double measuredRatePerMinute(const HeatingRate& rate);
// The SoC estimate.
std::optional<double> sessionAheadMinutes(std::optional<double> socPercent, std::optional<double> socAgeMs,
                                          const std::vector<SocSample>& socSamples, std::optional<double> requestedAmps,
                                          std::int64_t nowMs);
std::optional<double> estimateSocRate(const std::vector<SocSample>& samples, std::int64_t nowMs);

}  // namespace emcool::vcu
