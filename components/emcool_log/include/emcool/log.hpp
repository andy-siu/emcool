#pragma once

#include <cstdint>

namespace emcool {

// How many lines have been logged at each level, per tag, since boot — so a check can tell
// that a port warns exactly when the original does.
struct LogCounts {
  std::uint32_t info = 0;
  std::uint32_t warning = 0;
  std::uint32_t error = 0;
};
LogCounts logCountsFor(const char* tag);

// Host builds only: hands every line to `observer` as it is logged, so a check can compare a
// port's log lines with the original's. nullptr stops it.
using LogObserver = void (*)(char level, const char* tag, const char* message, void* context);
void setLogObserver(LogObserver observer, void* context);

[[gnu::format(printf, 2, 3)]] void logInfo(const char* tag, const char* format, ...);
[[gnu::format(printf, 2, 3)]] void logWarning(const char* tag, const char* format, ...);
[[gnu::format(printf, 2, 3)]] void logError(const char* tag, const char* format, ...);

}  // namespace emcool
