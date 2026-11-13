#pragma once

// The shape every frame decoder produces, and the byte readers they read fields with.
// BE = big-endian pair, LE = little-endian pair.

#include <cstddef>
#include <cstdint>
#include <span>

namespace emcool::can {

using FrameBytes = std::span<const std::uint8_t>;

// `key` always points at a string literal, so a value can outlive the frame it came from.
struct DecodedValue {
  const char* key;
  double value;
};

// Fixed capacity so decoding never touches the heap: the bus delivers thousands of frames a
// second and a PSRAM-less WROOM-32 has ~100 KB of heap to share with Bluetooth. The busiest
// frame, 0x102, yields 34 values; a push past capacity is dropped and flagged, never silent.
class DecodedValues {
 public:
  static constexpr std::size_t kCapacity = 48;

  void push(const char* key, double value) {
    if (count_ == kCapacity) {
      overflowed_ = true;
      return;
    }
    values_[count_] = DecodedValue{key, value};
    count_++;
  }

  void clear() {
    count_ = 0;
    overflowed_ = false;
  }

  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool overflowed() const { return overflowed_; }
  const DecodedValue& operator[](std::size_t index) const { return values_[index]; }
  const DecodedValue* begin() const { return values_; }
  const DecodedValue* end() const { return values_ + count_; }

 private:
  DecodedValue values_[kCapacity] = {};
  std::size_t count_ = 0;
  bool overflowed_ = false;
};

inline int signedByte(std::uint8_t byte) { return byte > 127 ? byte - 256 : byte; }

inline unsigned u16be(std::uint8_t high, std::uint8_t low) { return (unsigned{high} << 8) | low; }

inline int i16be(std::uint8_t high, std::uint8_t low) {
  const int value = (int{high} << 8) | low;
  return value > 32767 ? value - 65536 : value;
}

inline unsigned u16le(std::uint8_t low, std::uint8_t high) { return u16be(high, low); }

// Same two's-complement conversion as i16be with the bytes the other way round, so there
// is only one place to be wrong about the sign bit.
inline int i16le(std::uint8_t low, std::uint8_t high) { return i16be(high, low); }

inline unsigned bit(std::uint32_t word, unsigned index) { return (word >> index) & 1u; }

inline std::uint32_t u32le(FrameBytes data, std::size_t offset) {
  return std::uint32_t{data[offset]} | (std::uint32_t{data[offset + 1]} << 8) |
         (std::uint32_t{data[offset + 2]} << 16) | (std::uint32_t{data[offset + 3]} << 24);
}

inline std::uint32_t u32be(FrameBytes data, std::size_t offset) {
  return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16) |
         (std::uint32_t{data[offset + 2]} << 8) | std::uint32_t{data[offset + 3]};
}

inline std::uint32_t u24be(FrameBytes data, std::size_t offset) {
  return (std::uint32_t{data[offset]} << 16) | (std::uint32_t{data[offset + 1]} << 8) |
         std::uint32_t{data[offset + 2]};
}

// A field that does not start or end on a byte boundary, in the vehicle frames' bit
// numbering: bit N is byte N>>3, bit N&7, least-significant bit first (0x104 packs a 15-bit
// speed and a 15-bit rpm back to back). Callers stay under 32 bits.
std::uint32_t bitFieldLe(FrameBytes data, unsigned startBit, unsigned bitCount);

// JavaScript's Math.round: halves go towards +infinity, so -2.5 → -2. std::round would give
// -3, and `pack_kw` rounds a product that is negative whenever the pack is regenerating.
double jsRound(double value);

}  // namespace emcool::can
