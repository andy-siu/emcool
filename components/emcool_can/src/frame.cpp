#include "emcool/can/frame.hpp"

#include <cmath>

namespace emcool::can {

std::uint32_t bitFieldLe(FrameBytes data, unsigned startBit, unsigned bitCount) {
  std::uint32_t value = 0;
  for (unsigned offset = 0; offset < bitCount; offset++) {
    const unsigned absoluteBit = startBit + offset;
    value |= ((std::uint32_t{data[absoluteBit >> 3]} >> (absoluteBit & 7)) & 1u) << offset;
  }
  return value;
}

double jsRound(double value) {
  if (!std::isfinite(value)) {
    return value;
  }
  const double floor = std::floor(value);
  // Exact: value and its floor are within 1 of each other, so the subtraction cannot round.
  return value - floor >= 0.5 ? floor + 1.0 : floor;
}

}  // namespace emcool::can
