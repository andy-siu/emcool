#include "emcool/can/psu.hpp"

namespace emcool::can {

std::optional<PsuMonitor> decodePsuMonitor(std::span<const std::uint8_t> data) {
  if (data.size() < 6) return std::nullopt;
  PsuMonitor monitor;
  monitor.mainMv = static_cast<std::uint32_t>(data[0] | (data[1] << 8));
  monitor.standbyMv = static_cast<std::uint32_t>(data[2] | (data[3] << 8));
  monitor.loadMa = static_cast<std::uint32_t>(data[4] | (data[5] << 8));
  return monitor;
}

const char* describePsuState(std::uint8_t state) {
  switch (state) {
    case 0x00:
      return "rail off";
    case 0x03:
      return "rail on";
    case 0x02:
      return "not starting";
  }
  return "unknown";
}

}  // namespace emcool::can
