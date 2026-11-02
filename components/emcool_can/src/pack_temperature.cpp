#include "emcool/can/pack_temperature.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

#include "emcool/log.hpp"

// Ported from cool-eva src/can/pack-temperature.ts, warnings included: they are how a
// misconfigured BMS or setting gets noticed on a bike with no debugger attached.

namespace emcool::can {

namespace {

constexpr const char* kTag = "bms";
constexpr std::uint32_t kPackThermalFrameId = 0x660;
// 0x660's DLC: 3 before the VCU temperature offset exists, 8 with it (decode_bms.cpp).
constexpr std::size_t kOffsetConfigMinDlc = 5;
// 0x200 is 20 Hz and 0x660 1 Hz, so they're sampled up to a second apart and differ by a degree
// whenever the pack crosses one. A repointing error disagrees on every frame, usually by a lot.
constexpr std::uint32_t kEchoMismatchesBeforeWarning = 3;
constexpr std::uint32_t kClampGateFaultsBeforeWarning = 3;
constexpr double kClampGateClosed = 255;
constexpr double kClampGateOpen = 0;
constexpr double kRoundingDivideGate = 254;
// What the VCU is shown throughout the clamped band, and the VCU's limp threshold.
constexpr double kClampFloorC = 35;
constexpr double kLimpModeTempC = 55;

std::optional<double> find(const DecodedValues& decoded, std::string_view key) {
  for (const DecodedValue& value : decoded) {
    if (key == value.key) return value.value;
  }
  return std::nullopt;
}

}  // namespace

void PackTemperatureRouter::resolve(std::uint32_t id, FrameBytes data, DecodedValues& decoded,
                                    std::int64_t monotonicMs) {
  if (id == kPackThermalFrameId) {
    notePackThermalFrame(data.size());
    noteEchoAgreement(decoded);
    noteClampGate(decoded);
    return;
  }
  if (const std::optional<double> high = find(decoded, "batt_temp_hi_vcu")) {
    lastVcuTemperatureHigh_ = *high;
  }
  const std::optional<double> low = find(decoded, "batt_temp_lo_vcu");
  const std::optional<double> high = find(decoded, "batt_temp_hi_vcu");
  if (!low && !high) return;
  if (!firstVcuTemperatureAtMs_) {
    firstVcuTemperatureAtMs_ = monotonicMs;
  }
  if (!resolveVcuFrameOwnership(monotonicMs)) return;
  // In the order the decoder emitted the *_vcu keys, as cool-eva appends them.
  for (std::size_t index = 0, count = decoded.size(); index < count; index++) {
    const std::string_view key = decoded[index].key;
    if (key == "batt_temp_lo_vcu") {
      decoded.push("batt_temp_lo", decoded[index].value);
    } else if (key == "batt_temp_hi_vcu") {
      decoded.push("batt_temp_hi", decoded[index].value);
    }
  }
}

void PackTemperatureRouter::noteEchoAgreement(const DecodedValues& decoded) {
  const std::optional<double> echo = find(decoded, "batt_temp_hi_vcu_echo");
  if (!echo || !lastVcuTemperatureHigh_) return;
  if (*echo == *lastVcuTemperatureHigh_) {
    consecutiveEchoMismatches_ = 0;
    return;
  }
  consecutiveEchoMismatches_++;
  if (consecutiveEchoMismatches_ < kEchoMismatchesBeforeWarning || warnedEchoMismatch_) return;
  warnedEchoMismatch_ = true;
  logWarning(kTag,
             "*** 0x660 b7 (%g °C) and 0x200 b3 (%g °C) disagree on %u consecutive frames, but they read the "
             "same BMS memory. The flashed .bms config almost certainly points one of them at the wrong "
             "postprocessor slot. batt_temp_hi_vcu is what the VCU acts on, so treat it as the real one and "
             "re-check the config's CANTX signal addresses.",
             *echo, *lastVcuTemperatureHigh_, static_cast<unsigned>(consecutiveEchoMismatches_));
}

void PackTemperatureRouter::noteClampGate(const DecodedValues& decoded) {
  const std::optional<double> gate = find(decoded, "clamp_gate");
  if (!gate) return;
  noteClampGateIsAMask(*gate);
  if (const std::optional<double> trueTemperatureHigh = find(decoded, "batt_temp_hi")) {
    noteClampGateRegime(*gate, *trueTemperatureHigh);
  }
}

void PackTemperatureRouter::noteClampGateIsAMask(double gate) {
  if (gate == kClampGateClosed || gate == kClampGateOpen) {
    consecutiveNonMaskGates_ = 0;
    return;
  }
  consecutiveNonMaskGates_++;
  if (consecutiveNonMaskGates_ < kClampGateFaultsBeforeWarning || warnedClampGateNotAMask_) return;
  warnedClampGateNotAMask_ = true;
  const char* diagnosis =
      gate == kRoundingDivideGate
          ? "That is what a ROUNDING Divide operator produces where the config assumes a truncating one. The "
            "mask then drops bit 0 of the amount, so the temperature the VCU is shown is up to 1 °C off — check "
            "batt_temp_hi_vcu against 35 while the pack is in the clamped band."
          : "Most likely this pack is still on 14-signbit-clamp, where b5 is (true pack temp high − 35): compare "
            "clamp_gate with batt_temp_hi − 35 to confirm. If that is it, this run's clamp_gate rows are that "
            "difference rather than a gate, and they are reconstructible from batt_temp_hi.";
  logWarning(kTag,
             "*** 0x660 b5 read %g on %u consecutive frames, but under 15-bounded-clamp it can only be %g (clamp "
             "active) or %g (true temperature being reported). *** %s batt_temp_lo/batt_temp_hi are untouched by "
             "this either way — b3/b4 carry the true temperatures under both configs.",
             gate, static_cast<unsigned>(consecutiveNonMaskGates_), kClampGateClosed, kClampGateOpen, diagnosis);
}

void PackTemperatureRouter::noteClampGateRegime(double gate, double trueTemperatureHigh) {
  const bool inClampedBand = trueTemperatureHigh > kClampFloorC && trueTemperatureHigh < kLimpModeTempC;
  if (!inClampedBand || gate != kClampGateOpen) {
    consecutiveOpenGatesInClampBand_ = 0;
    return;
  }
  consecutiveOpenGatesInClampBand_++;
  if (consecutiveOpenGatesInClampBand_ < kClampGateFaultsBeforeWarning || warnedClampGateStuckOpen_) return;
  warnedClampGateStuckOpen_ = true;
  logWarning(kTag,
             "0x660 b5 has been %g for %u consecutive frames with the pack at a true %g °C — above the %g °C clamp "
             "floor and below the %g °C limp threshold, so the gate should be %g. It is not closing, so nothing is "
             "being subtracted and the VCU is seeing the true temperature. This fails SAFE — limp mode and every "
             "BMS protection still work — but the DC-charge derate relief the config exists for is not happening. "
             "Likeliest cause: the postprocessor's Divide treats its operand as signed, so (true − 55) never "
             "yields a quotient of 1 below 55 °C.",
             kClampGateOpen, static_cast<unsigned>(consecutiveOpenGatesInClampBand_), trueTemperatureHigh,
             kClampFloorC, kLimpModeTempC, kClampGateClosed);
}

void PackTemperatureRouter::notePackThermalFrame(std::size_t frameLength) {
  anyThermalFrameSeen_ = true;
  if (frameLength < kOffsetConfigMinDlc) {
    noteShortThermalFrame();
    return;
  }
  consecutiveShortThermalFrames_ = 0;
  source_ = Source::kThermalFrame;
  if (customBmsConfigExpected_ || warnedThermalFrameUnexpected_) return;
  warnedThermalFrameUnexpected_ = true;
  logError(kTag,
           "*** CUSTOM_BMS_CONFIG is not set, but extended 0x660 frames (DLC >= %zu) are arriving. *** The custom "
           "BMS config IS flashed, so 0x200's temperature bytes are the VCU's LOWERED view, not the truth. Taking "
           "batt_temp_lo/batt_temp_hi from 0x660. If 0x200 had already been flowing for %lld ms when this frame "
           "arrived, the keys were briefly fed from 0x200 and those rows are cold by whatever the live config "
           "subtracts — up to 19 °C under the bounded clamp, at a true 54 °C. Set CUSTOM_BMS_CONFIG to 1 (serial "
           "console: set CUSTOM_BMS_CONFIG 1) and restart.",
           kOffsetConfigMinDlc, static_cast<long long>(kThermalFrameWaitMs));
}

void PackTemperatureRouter::noteShortThermalFrame() {
  consecutiveShortThermalFrames_++;
  if (consecutiveShortThermalFrames_ < kShortThermalFramesToTrust) return;
  source_ = Source::kVcuFrame;
  if (!customBmsConfigExpected_ || warnedThermalFrameShort_) return;
  warnedThermalFrameShort_ = true;
  logWarning(kTag,
             "CUSTOM_BMS_CONFIG is set, but 0x660 arrives in its short form (DLC < %zu) — that is the extended "
             "config WITHOUT the VCU temperature offset. Taking batt_temp_lo/batt_temp_hi from 0x200, which is "
             "correct for that config. Set CUSTOM_BMS_CONFIG to 0 to silence this.",
             kOffsetConfigMinDlc);
}

// Whether 0x200's bytes are KNOWN to be the true temperature now, committing the decision:
// the stock case latches and logs once. Never true on a guess.
bool PackTemperatureRouter::resolveVcuFrameOwnership(std::int64_t monotonicMs) {
  if (source_ != Source::kUnknown) return source_ == Source::kVcuFrame;
  if (monotonicMs - *firstVcuTemperatureAtMs_ < kThermalFrameWaitMs) return false;
  char missing[200];
  if (customBmsConfigExpected_) {
    if (warnedThermalFrameMissing_) return false;
    warnedThermalFrameMissing_ = true;
    logWarning(kTag,
               "CUSTOM_BMS_CONFIG is set but %s, so nothing on the bus carries the true pack temperature. "
               "batt_temp_lo/batt_temp_hi stay UNLOGGED until a 0x660 turns up — under this config 0x200's bytes "
               "are the VCU's lowered view, not the truth. batt_temp_lo_vcu/batt_temp_hi_vcu keep logging "
               "throughout. If this pack is in fact stock, set CUSTOM_BMS_CONFIG to 0 and the true keys resume "
               "from 0x200.",
               describeMissingThermalFrame(missing, sizeof missing));
    return false;
  }
  source_ = Source::kVcuFrame;
  if (anyThermalFrameSeen_) {
    // A 0x660 exists, so this is not a stock pack; don't claim it is.
    logInfo(kTag, "%s — batt_temp_lo/batt_temp_hi now come from 0x200.",
            describeMissingThermalFrame(missing, sizeof missing));
  } else {
    logInfo(kTag, "%s — stock config confirmed, batt_temp_lo/batt_temp_hi now come from 0x200.",
            describeMissingThermalFrame(missing, sizeof missing));
  }
  return true;
}

const char* PackTemperatureRouter::describeMissingThermalFrame(char* buffer, std::size_t size) const {
  if (anyThermalFrameSeen_) {
    std::snprintf(buffer, size,
                  "a 0x660 was seen but not corroborated within %lld ms of the first 0x200 (%u consecutive short "
                  "frames are needed)",
                  static_cast<long long>(kThermalFrameWaitMs), static_cast<unsigned>(kShortThermalFramesToTrust));
  } else {
    std::snprintf(buffer, size, "no 0x660 arrived within %lld ms of the first 0x200",
                  static_cast<long long>(kThermalFrameWaitMs));
  }
  return buffer;
}

}  // namespace emcool::can
