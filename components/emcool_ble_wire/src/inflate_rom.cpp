// The ESP32 backend of inflateRaw(): the miniz tinfl inflater in the chip's ROM.

#include <cstdlib>

#include "emcool/ble/deflate.hpp"
#include "emcool/log.hpp"
#include "miniz.h"

namespace emcool::ble {

std::optional<std::size_t> inflateRaw(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
  // ~11 KB of decode tables, on the heap only while a message is being inflated: phone →
  // ESP32 traffic is a handshake and the odd request, so it isn't worth holding.
  auto* decompressor = static_cast<tinfl_decompressor*>(std::malloc(sizeof(tinfl_decompressor)));
  if (decompressor == nullptr) {
    logWarning("ble-wire", "no memory to inflate a %zu-byte message", in.size());
    return std::nullopt;
  }
  tinfl_init(decompressor);
  std::size_t inBytes = in.size();
  std::size_t outBytes = out.size();
  // No TINFL_FLAG_PARSE_ZLIB_HEADER: raw DEFLATE. The whole output buffer is the window.
  const tinfl_status status = tinfl_decompress(decompressor, in.data(), &inBytes, out.data(), out.data(), &outBytes,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  std::free(decompressor);
  if (status != TINFL_STATUS_DONE) {
    return std::nullopt;
  }
  return outBytes;
}

}  // namespace emcool::ble
