#include "system_time.hpp"

#include <sys/time.h>

#include <cerrno>
#include <cmath>

#include "emcool/log.hpp"
#include "esp_timer.h"

namespace emcool {

std::int64_t monotonicMs() { return esp_timer_get_time() / 1000; }

std::int64_t wallClockMs() {
  timeval now = {};
  gettimeofday(&now, nullptr);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

bool stepWallClock(double epochSeconds) {
  const double wholeSeconds = std::floor(epochSeconds);
  timeval target = {};
  target.tv_sec = static_cast<time_t>(wholeSeconds);
  target.tv_usec = static_cast<suseconds_t>((epochSeconds - wholeSeconds) * 1e6);
  if (settimeofday(&target, nullptr) != 0) {
    logError("clock", "settimeofday failed (errno %d)", errno);
    return false;
  }
  return true;
}

}  // namespace emcool
