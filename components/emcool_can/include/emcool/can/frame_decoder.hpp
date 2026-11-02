#pragma once

// Turns one broadcast CAN frame into the (signal key, value) pairs it carries. A port of
// cool-eva src/can/decode.ts and the modules it routes to; unknown ids yield nothing.
//
// Pure apart from two pieces of state cool-eva also keeps: the GPS fix that spans three
// 0x410 sub-frames, and the once-per-axis attitude range warning. Both live in this object
// rather than in globals, so replaying a second capture means constructing a fresh one.

#include <cstdint>

#include "emcool/can/frame.hpp"
#include "emcool/can/gps.hpp"

namespace emcool::can {

struct AxisRangeWatch {
  std::uint32_t consecutiveOutOfRange = 0;
  bool warned = false;
};

class FrameDecoder {
 public:
  FrameDecoder() : suppressedFixWatcher_(busGps_, "can") {}
  FrameDecoder(const FrameDecoder&) = delete;
  FrameDecoder& operator=(const FrameDecoder&) = delete;

  // Appends to `out` without clearing it, so a caller can batch several frames.
  void decode(std::uint32_t id, FrameBytes data, DecodedValues& out);

 private:
  GpsMessageDecoder busGps_;
  SuppressedFixWatcher suppressedFixWatcher_;
  AxisRangeWatch rollRangeWatch_;
  AxisRangeWatch pitchRangeWatch_;
};

}  // namespace emcool::can
