#include <numbers>

#include "decoders.hpp"
#include "emcool/can/gps.hpp"

// 0x410 message type 3: the dash's speed and the motor's torque, with power derived from
// torque × rpm. Ported from cool-eva src/can/hub-output.ts and src/hub/output.ts.

namespace emcool::can {

namespace {

constexpr std::uint8_t kHubOutputType = 3;

}  // namespace

void decodeHubOutputFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < kHubFrameSize || data[0] != kHubOutputType || data[1] != 0xff) {
    return;
  }
  const int revolutionsPerMinute = i16le(data[4], data[5]);
  const int torqueNm = i16le(data[6], data[7]);
  out.push("dash_speed_kmh", u16le(data[2], data[3]));
  out.push("motor_torque_can_nm", torqueNm);
  // Left to right, as JavaScript evaluates `torqueNm * 2 * Math.PI * rpm / 60000`.
  out.push("motor_power_can_kw", ((torqueNm * 2.0) * std::numbers::pi * revolutionsPerMinute) / 60000.0);
}

}  // namespace emcool::can
