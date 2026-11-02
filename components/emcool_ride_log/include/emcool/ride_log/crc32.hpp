#pragma once

// CRC-32 as zlib and Node's zlib.crc32 compute it (reflected, polynomial 0xEDB88320), so
// the laptop reader can check records with the standard library rather than a copy of this.

#include <cstddef>
#include <cstdint>

namespace emcool::ride_log {

// Continue a running CRC by passing the previous result as `crc`; start from 0.
std::uint32_t crc32(const std::uint8_t* data, std::size_t length, std::uint32_t crc = 0);

}  // namespace emcool::ride_log
