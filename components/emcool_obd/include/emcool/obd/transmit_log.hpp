#pragma once

// cool-eva src/can/transmit-log.ts: a refused frame is logged at once, then at most once per
// 5 s per sender, with the count it held back on the next line it does log. A bus that refuses
// everything would otherwise log twice a second from the poller alone.

#include <cstdint>
#include <optional>
#include <string>

namespace emcool::obd {

class TransmitFailureLog {
 public:
  static constexpr std::int64_t kIntervalMs = 5000;

  explicit TransmitFailureLog(const char* tag) : tag_(tag) {}

  void log(std::int64_t nowMs, const std::string& message, const std::string& reason);

 private:
  const char* tag_;
  std::optional<std::int64_t> lastLoggedMs_;
  std::uint32_t suppressed_ = 0;
};

// cool-eva's loggers are module-level, so clear-dtcs's PID and trouble-code reads count against
// the poller's. One of these per ObdStack, shared the same way.
struct TransmitLogs {
  TransmitFailureLog obd{"obd"};
  TransmitFailureLog troubleCodes{"obd-dtc"};
};

}  // namespace emcool::obd
