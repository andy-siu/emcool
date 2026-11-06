#include "emcool/obd/transmit_log.hpp"

#include "emcool/log.hpp"

namespace emcool::obd {

void TransmitFailureLog::log(std::int64_t nowMs, const std::string& message, const std::string& reason) {
  if (lastLoggedMs_ && nowMs - *lastLoggedMs_ < kIntervalMs) {
    suppressed_ += 1;
    return;
  }
  const std::string heldBack =
      suppressed_ > 0 ? " (" + std::to_string(suppressed_) + " transmit failures suppressed)" : "";
  logError(tag_, "%s%s: %s", message.c_str(), heldBack.c_str(), reason.c_str());
  lastLoggedMs_ = nowMs;
  suppressed_ = 0;
}

}  // namespace emcool::obd
