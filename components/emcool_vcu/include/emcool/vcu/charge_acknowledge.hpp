#pragma once

// Did a charge-current command take? Pure — the commanded amps, when it was sent, and the samples
// of the bike's charge request around then, in; a verdict out. A reduction is the only thing that
// proves a command took, because the station caps what actually flows.

#include <cstdint>
#include <string>
#include <vector>

namespace emcool::vcu {

inline constexpr std::int64_t kAckTimeoutMs = 10000;
inline constexpr std::int64_t kAckSettleMs = 1000;
inline constexpr std::int64_t kAckSampleHistoryMs = 60000;

struct ChargeAckVerdict {
  enum class Kind : std::uint8_t {
    kWaiting,
    kTook,
    kNotAcknowledged,
    kStationLimited,
    kNoChangeExpected,
    kSuperseded,
    kNoEvidence,
  };
  Kind kind = Kind::kNoEvidence;
  // kWaiting
  std::int64_t elapsedMs = 0;
  // kTook
  double fromAmps = 0;
  double toAmps = 0;
  std::int64_t latencyMs = 0;
  // kNotAcknowledged
  double heldAmps = 0;
  // kStationLimited
  double askedAmps = 0;
  double gotAmps = 0;
  // kNoChangeExpected
  double atAmps = 0;
};

struct AckSample {
  std::int64_t atMs;
  double amps;
};

struct ChargeAckInput {
  double commandedAmps = 0;
  std::int64_t sentAtMs = 0;
  std::int64_t nowMs = 0;
  std::vector<AckSample> samples;
  bool hasSuperseded = false;
  std::int64_t supersededAtMs = 0;
  std::int64_t settleMs = kAckSettleMs;
};

ChargeAckVerdict judgeChargeCommand(const ChargeAckInput& input);
int chargeAckCode(ChargeAckVerdict::Kind kind);
std::string describeChargeAck(const ChargeAckVerdict& verdict, double commandedAmps);

}  // namespace emcool::vcu
