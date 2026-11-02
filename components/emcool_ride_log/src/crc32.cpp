#include "emcool/ride_log/crc32.hpp"

#include <array>

namespace emcool::ride_log {

namespace {

constexpr std::array<std::uint32_t, 256> makeTable() {
  std::array<std::uint32_t, 256> table = {};
  for (std::uint32_t index = 0; index < 256; index++) {
    std::uint32_t value = index;
    for (int bitIndex = 0; bitIndex < 8; bitIndex++) {
      value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = makeTable();

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t length, std::uint32_t crc) {
  crc = ~crc;
  for (std::size_t index = 0; index < length; index++) {
    crc = kTable[(crc ^ data[index]) & 0xffu] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace emcool::ride_log
