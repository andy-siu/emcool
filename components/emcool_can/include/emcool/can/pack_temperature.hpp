#pragma once

// Decides which frame may write batt_temp_lo / batt_temp_hi, the TRUE pack temperature. A port
// of cool-eva src/can/pack-temperature.ts, the one config-dependent step in decoding:
//
// - On a stock Energica the truth is 0x200 b0/b3, which the decoder emits as *_vcu.
// - With the custom LiBAL BMS config flashed, those bytes are the VCU's lowered view, and the
//   truth moves to the long 0x660, which the decoder emits as batt_temp_lo/hi itself.
//
// No frame announces the config, so it is established from what arrives:
// - a long 0x660 proves the offset config and carries the truth itself;
// - two consecutive short ones prove the extended config without the offset, so 0x200 is true;
// - no 0x660 within 5 s of the first 0x200 means a stock pack.
// Until one of those holds, the keys are not emitted at all. A gap reads as "not known yet";
// a lowered value under the true key would read as a plunge no one could tell from a failing
// sensor. CUSTOM_BMS_CONFIG says which config to expect, but the frames win: the setting only
// decides the case the bus can't answer, and which mismatch gets warned about. cool-eva
// docs/can-decode-findings.md § "0x200 / 0x660" has the configs and the evidence.

#include <cstdint>
#include <optional>

#include "emcool/can/frame.hpp"

namespace emcool::can {

class PackTemperatureRouter {
 public:
  static constexpr std::int64_t kThermalFrameWaitMs = 5000;
  static constexpr std::uint32_t kShortThermalFramesToTrust = 2;

  explicit PackTemperatureRouter(bool customBmsConfigExpected) : customBmsConfigExpected_(customBmsConfigExpected) {}

  // Call with each frame's decoded values: appends batt_temp_lo/hi to a 0x200's values once
  // 0x200 is known to carry the truth, and watches 0x660 for what the config is.
  void resolve(std::uint32_t id, FrameBytes data, DecodedValues& decoded, std::int64_t monotonicMs);

 private:
  enum class Source : std::uint8_t { kUnknown, kVcuFrame, kThermalFrame };

  void notePackThermalFrame(std::size_t frameLength);
  void noteShortThermalFrame();
  void noteEchoAgreement(const DecodedValues& decoded);
  void noteClampGate(const DecodedValues& decoded);
  void noteClampGateIsAMask(double gate);
  void noteClampGateRegime(double gate, double trueTemperatureHigh);
  bool resolveVcuFrameOwnership(std::int64_t monotonicMs);
  const char* describeMissingThermalFrame(char* buffer, std::size_t size) const;

  const bool customBmsConfigExpected_;
  Source source_ = Source::kUnknown;
  std::optional<std::int64_t> firstVcuTemperatureAtMs_;
  std::uint32_t consecutiveShortThermalFrames_ = 0;
  bool anyThermalFrameSeen_ = false;
  bool warnedThermalFrameMissing_ = false;
  bool warnedThermalFrameUnexpected_ = false;
  bool warnedThermalFrameShort_ = false;
  std::optional<double> lastVcuTemperatureHigh_;
  std::uint32_t consecutiveEchoMismatches_ = 0;
  bool warnedEchoMismatch_ = false;
  std::uint32_t consecutiveNonMaskGates_ = 0;
  std::uint32_t consecutiveOpenGatesInClampBand_ = 0;
  bool warnedClampGateNotAMask_ = false;
  bool warnedClampGateStuckOpen_ = false;
};

}  // namespace emcool::can
