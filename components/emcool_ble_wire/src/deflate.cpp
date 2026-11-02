#include "emcool/ble/deflate.hpp"

#include <algorithm>
#include <cstring>

// RFC 1951 §3.2.5 (length and distance codes) and §3.2.6 (the fixed Huffman codes).

namespace emcool::ble {

namespace {

constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxMatch = 258;
constexpr unsigned kMaxChain = 32;

constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtraBits[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                             33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistanceExtraBits[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Writes bits least-significant first, as DEFLATE packs them, refusing to run past the end.
class BitWriter {
 public:
  explicit BitWriter(std::span<std::uint8_t> out) : out_(out) {}

  void put(std::uint32_t value, unsigned count) {
    buffer_ |= static_cast<std::uint64_t>(value) << bitCount_;
    bitCount_ += count;
    while (bitCount_ >= 8) {
      emit(static_cast<std::uint8_t>(buffer_));
      buffer_ >>= 8;
      bitCount_ -= 8;
    }
  }

  // Huffman codes are defined most-significant bit first, so they go in reversed.
  void putCode(std::uint32_t code, unsigned length) {
    std::uint32_t reversed = 0;
    for (unsigned index = 0; index < length; index++) {
      reversed |= ((code >> index) & 1u) << (length - 1 - index);
    }
    put(reversed, length);
  }

  void flush() {
    if (bitCount_ > 0) {
      emit(static_cast<std::uint8_t>(buffer_));
      buffer_ = 0;
      bitCount_ = 0;
    }
  }

  bool overflowed() const { return overflowed_; }
  std::size_t length() const { return length_; }

 private:
  void emit(std::uint8_t byte) {
    if (length_ == out_.size()) {
      overflowed_ = true;
      return;
    }
    out_[length_++] = byte;
  }

  std::span<std::uint8_t> out_;
  std::uint64_t buffer_ = 0;
  unsigned bitCount_ = 0;
  std::size_t length_ = 0;
  bool overflowed_ = false;
};

void putLiteralOrLength(BitWriter& writer, unsigned symbol) {
  if (symbol <= 143) {
    writer.putCode(0x30 + symbol, 8);
  } else if (symbol <= 255) {
    writer.putCode(0x190 + (symbol - 144), 9);
  } else if (symbol <= 279) {
    writer.putCode(symbol - 256, 7);
  } else {
    writer.putCode(0xc0 + (symbol - 280), 8);
  }
}

void putMatch(BitWriter& writer, std::size_t length, std::size_t distance) {
  unsigned lengthCode = 28;
  while (kLengthBase[lengthCode] > length) {
    lengthCode--;
  }
  putLiteralOrLength(writer, 257 + lengthCode);
  writer.put(static_cast<std::uint32_t>(length - kLengthBase[lengthCode]), kLengthExtraBits[lengthCode]);
  unsigned distanceCode = 29;
  while (kDistanceBase[distanceCode] > distance) {
    distanceCode--;
  }
  writer.putCode(distanceCode, 5);
  writer.put(static_cast<std::uint32_t>(distance - kDistanceBase[distanceCode]), kDistanceExtraBits[distanceCode]);
}

unsigned hashAt(std::span<const std::uint8_t> in, std::size_t position, unsigned bits) {
  const std::uint32_t value =
      (std::uint32_t{in[position]} << 16) | (std::uint32_t{in[position + 1]} << 8) | in[position + 2];
  return (value * 2654435761u) >> (32 - bits);
}

}  // namespace

std::size_t Deflater::compress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
  if (in.size() > 0xffff) {
    return 0;
  }
  const std::size_t fixedLength = compressFixed(in, out);
  const std::size_t storedLength = in.size() + 5;
  if (fixedLength != 0 && fixedLength <= storedLength) {
    return fixedLength;
  }
  if (out.size() < storedLength) {
    return 0;
  }
  // A final stored block: header bits 1 (BFINAL) and 00 (BTYPE), padded to the byte,
  // then LEN and its one's complement, then the bytes as they are.
  const auto length = static_cast<std::uint16_t>(in.size());
  out[0] = 0x01;
  out[1] = static_cast<std::uint8_t>(length);
  out[2] = static_cast<std::uint8_t>(length >> 8);
  out[3] = static_cast<std::uint8_t>(~length);
  out[4] = static_cast<std::uint8_t>(~length >> 8);
  if (!in.empty()) {
    std::memcpy(out.data() + 5, in.data(), in.size());
  }
  return storedLength;
}

std::size_t Deflater::compressFixed(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
  std::fill(std::begin(head_), std::end(head_), kNoPosition);
  BitWriter writer(out);
  writer.put(1, 1);  // BFINAL
  writer.put(1, 2);  // BTYPE 01: fixed Huffman codes

  std::size_t position = 0;
  while (position < in.size() && !writer.overflowed()) {
    std::size_t bestLength = 0;
    std::size_t bestDistance = 0;
    if (position + kMinMatch <= in.size()) {
      const std::size_t longest = std::min(kMaxMatch, in.size() - position);
      std::uint16_t candidate = head_[hashAt(in, position, kHashBits)];
      for (unsigned chain = 0; candidate != kNoPosition && chain < kMaxChain; chain++) {
        if (candidate >= position) break;
        const std::size_t distance = position - candidate;
        if (distance > kWindowBytes) break;
        std::size_t length = 0;
        while (length < longest && in[candidate + length] == in[position + length]) {
          length++;
        }
        if (length > bestLength) {
          bestLength = length;
          bestDistance = distance;
          if (length == longest) break;
        }
        const std::uint16_t next = previous_[candidate % kWindowBytes];
        // A slot reused by a later position, or one past the window, ends the chain.
        if (next == kNoPosition || next >= candidate) break;
        candidate = next;
      }
    }
    if (bestLength >= kMinMatch) {
      putMatch(writer, bestLength, bestDistance);
      for (std::size_t covered = 0; covered < bestLength; covered++) {
        insert(in, position + covered);
      }
      position += bestLength;
    } else {
      putLiteralOrLength(writer, in[position]);
      insert(in, position);
      position++;
    }
  }
  putLiteralOrLength(writer, 256);
  writer.flush();
  return writer.overflowed() ? 0 : writer.length();
}

void Deflater::insert(std::span<const std::uint8_t> in, std::size_t position) {
  if (position + kMinMatch > in.size()) return;
  const unsigned hash = hashAt(in, position, kHashBits);
  previous_[position % kWindowBytes] = head_[hash];
  head_[hash] = static_cast<std::uint16_t>(position);
}

}  // namespace emcool::ble
