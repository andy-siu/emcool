// The host backend of inflateRaw(), on the system zlib.

#include <zlib.h>

#include "emcool/ble/deflate.hpp"

namespace emcool::ble {

std::optional<std::size_t> inflateRaw(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
  z_stream stream = {};
  // Negative window bits: raw DEFLATE, no zlib header or checksum.
  if (inflateInit2(&stream, -15) != Z_OK) {
    return std::nullopt;
  }
  stream.next_in = const_cast<Bytef*>(in.data());
  stream.avail_in = static_cast<uInt>(in.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = inflate(&stream, Z_FINISH);
  const std::size_t produced = stream.total_out;
  inflateEnd(&stream);
  if (result != Z_STREAM_END) {
    return std::nullopt;
  }
  return produced;
}

}  // namespace emcool::ble
