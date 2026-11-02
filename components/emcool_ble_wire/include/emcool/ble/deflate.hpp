#pragma once

// Raw DEFLATE (RFC 1951, no zlib header) — what the phone's DecompressionStream("deflate-raw")
// reads. ESP-IDF's ROM compressor needs ~165 KB of state even in its low-memory build, so this
// is a small one: a single block with the fixed Huffman codes and greedy LZ77 over a hash
// chain, or a stored block when that comes out smaller. ~10 KB of tables, no allocation.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace emcool::ble {

class Deflater {
 public:
  // Back-references reach this far; the link's messages are a few KB.
  static constexpr std::size_t kWindowBytes = 4096;

  // The compressed length, or 0 if `out` is too small. in.size() + 5 bytes (the stored-block
  // fallback) always suffice, for inputs up to 65535 bytes.
  std::size_t compress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

 private:
  static constexpr unsigned kHashBits = 10;
  static constexpr std::uint16_t kNoPosition = 0xffff;

  std::size_t compressFixed(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);
  void insert(std::span<const std::uint8_t> in, std::size_t position);

  std::uint16_t head_[1u << kHashBits] = {};
  std::uint16_t previous_[kWindowBytes] = {};
};

// Raw DEFLATE in, at most out.size() bytes out. nullopt if the stream is malformed or its
// output would not fit. ESP32: the ROM's tinfl. Host: zlib.
std::optional<std::size_t> inflateRaw(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

}  // namespace emcool::ble
