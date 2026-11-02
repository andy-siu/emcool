#include <cstdio>
#include <cstdlib>

#include "decoders.hpp"
#include "emcool/log.hpp"

// 0x102 b4-5 / b6-7: the attitude sensor's roll and pitch in 0.1°, NOT the accelerations
// the .xdbc calls them. Ported from cool-eva src/can/attitude.ts, which has the argument.

namespace emcool::can {

namespace {

constexpr double kDecidegreesPerDegree = 10;
// An atan2 cannot leave ±180.0°, so a count past this means the layout has changed.
constexpr int kMaxDecidegrees = 1800;
constexpr std::size_t kMinFrameLength = 8;
constexpr std::uint32_t kOutOfRangeFramesBeforeWarning = 5;

void noteOutOfRange(const char* key, int decidegrees, AxisRangeWatch& watch, FrameBytes data) {
  watch.consecutiveOutOfRange++;
  if (watch.consecutiveOutOfRange < kOutOfRangeFramesBeforeWarning || watch.warned) return;
  watch.warned = true;
  char frameHex[2 * 8 + 1] = {};
  for (std::size_t index = 0; index < data.size() && index < 8; index++) {
    std::snprintf(frameHex + 2 * index, 3, "%02x", data[index]);
  }
  logWarning("attitude",
             "*** 0x102 %s read %d (%.1f°) on %u consecutive frames, outside the ±%.0f° an atan2 can "
             "produce. *** b4-7 are no longer the attitude angles this decoder assumes — most likely "
             "Energica changed 0x102's layout, or the frame is not from the VCU at all. Dropping the "
             "sample. Frame: %s. See cool-eva src/can/attitude.ts.",
             key, decidegrees, decidegrees / kDecidegreesPerDegree,
             static_cast<unsigned>(watch.consecutiveOutOfRange), kMaxDecidegrees / kDecidegreesPerDegree,
             frameHex);
}

void addAngle(const char* key, int decidegrees, AxisRangeWatch& watch, FrameBytes data, DecodedValues& out) {
  if (std::abs(decidegrees) > kMaxDecidegrees) {
    noteOutOfRange(key, decidegrees, watch, data);
    return;
  }
  watch.consecutiveOutOfRange = 0;
  out.push(key, decidegrees / kDecidegreesPerDegree);
}

}  // namespace

void decodeAttitudeFrame(FrameBytes data, AxisRangeWatch& roll, AxisRangeWatch& pitch, DecodedValues& out) {
  if (data.size() < kMinFrameLength) return;
  addAngle("attitude_roll_deg", i16le(data[4], data[5]), roll, data, out);
  addAngle("attitude_pitch_deg", i16le(data[6], data[7]), pitch, data, out);
}

}  // namespace emcool::can
