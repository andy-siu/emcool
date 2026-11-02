#pragma once

// The two clocks. Durations always come from the monotonic one: the wall clock gets stepped
// to GPS time, and an interval spanning a step would come out wrong.

#include <cstdint>

namespace emcool {

std::int64_t monotonicMs();
std::int64_t wallClockMs();
bool stepWallClock(double epochSeconds);

}  // namespace emcool
