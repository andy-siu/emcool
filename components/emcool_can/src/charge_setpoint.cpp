#include "decoders.hpp"

// 0x121, the dash's charge commands. EVENTS, not streams: they fire only when the rider moves
// the dial. Ported from cool-eva src/can/charge-setpoint.ts and src/can/charge-soc-limit.ts.

namespace emcool::can {

namespace {

constexpr std::uint8_t kDcCurrentLimitOpcode = 0x18;
constexpr std::uint8_t kAcCurrentLimitOpcode = 0x1a;
constexpr std::uint8_t kSocLimitOpcode = 0x2c;
constexpr std::uint8_t kSeparatorByte = 0xff;
constexpr std::uint8_t kLimitInForce = 1;
constexpr unsigned kMaxSocLimitPercent = 100;

}  // namespace

void decodeChargeSetpointFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() != 8 || data[5] != 0 || data[6] != 0 || data[7] != 0) {
    return;
  }
  const std::uint8_t opcode = data[0];
  if ((opcode != kDcCurrentLimitOpcode && opcode != kAcCurrentLimitOpcode) || data[1] != kSeparatorByte) {
    return;
  }
  if (data[3] != kLimitInForce) {
    return;
  }
  const std::uint8_t selected = data[2];
  const std::uint8_t ceiling = data[4];
  if (selected < 1 || selected > ceiling) {
    return;
  }
  if (opcode == kDcCurrentLimitOpcode) {
    out.push("dc_charge_limit_selected_a", selected);
    return;
  }
  out.push("ac_charge_ceiling_a", ceiling);
}

void decodeChargeSocLimitFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() != 8 || data[0] != kSocLimitOpcode || data[1] != kSeparatorByte) {
    return;
  }
  if (data[3] != 0 || data[4] != 0 || data[5] != 0 || data[6] != 0 || data[7] != 0) {
    return;
  }
  const std::uint8_t percent = data[2];
  if (percent > kMaxSocLimitPercent) {
    return;
  }
  out.push("charge_soc_limit_pct", percent);
}

}  // namespace emcool::can
