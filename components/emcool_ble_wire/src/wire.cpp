#include "emcool/ble/wire.hpp"

#include <algorithm>
#include <cstring>

namespace emcool::ble {

namespace {

constexpr std::uint8_t kFrameFirst = 1;
constexpr std::uint8_t kFrameLast = 2;
constexpr std::uint8_t kEnvelopeClear = 0;
constexpr std::uint8_t kEnvelopeSealed = 1;
constexpr std::size_t kSealedPrefixBytes = 9;
constexpr std::size_t kMessageHeaderBytes = 5;
// Nonces in this protocol are 16 bytes; anything longer is refused rather than hashed.
constexpr std::size_t kMaxNonceBytes = 64;

void putBigEndian32(std::uint8_t* destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value >> 24);
  destination[1] = static_cast<std::uint8_t>(value >> 16);
  destination[2] = static_cast<std::uint8_t>(value >> 8);
  destination[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t getBigEndian32(const std::uint8_t* source) {
  return (std::uint32_t{source[0]} << 24) | (std::uint32_t{source[1]} << 16) | (std::uint32_t{source[2]} << 8) |
         std::uint32_t{source[3]};
}

void sealedPrefix(std::uint64_t counter, std::uint8_t* prefix) {
  prefix[0] = kEnvelopeSealed;
  putBigEndian32(prefix + 1, static_cast<std::uint32_t>(counter >> 32));
  putBigEndian32(prefix + 5, static_cast<std::uint32_t>(counter));
}

// [direction][0 0 0][counter u64 BE], as the dashboard's ivFor() builds it.
GcmIv ivFor(Direction direction, std::uint64_t counter) {
  GcmIv iv = {};
  iv[0] = static_cast<std::uint8_t>(direction);
  putBigEndian32(iv.data() + 4, static_cast<std::uint32_t>(counter >> 32));
  putBigEndian32(iv.data() + 8, static_cast<std::uint32_t>(counter));
  return iv;
}

std::span<const std::uint8_t> bytesOf(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

bool labelledHmac(std::string_view secret, std::string_view label, std::string_view serverNonceHex,
                  std::string_view clientNonceHex, Sha256Digest& out) {
  if (serverNonceHex.size() % 2 != 0 || clientNonceHex.size() % 2 != 0 ||
      serverNonceHex.size() / 2 > kMaxNonceBytes || clientNonceHex.size() / 2 > kMaxNonceBytes) {
    return false;
  }
  std::uint8_t serverNonce[kMaxNonceBytes];
  std::uint8_t clientNonce[kMaxNonceBytes];
  const std::span<std::uint8_t> serverBytes(serverNonce, serverNonceHex.size() / 2);
  const std::span<std::uint8_t> clientBytes(clientNonce, clientNonceHex.size() / 2);
  if (!fromHex(serverNonceHex, serverBytes) || !fromHex(clientNonceHex, clientBytes)) {
    return false;
  }
  return hmacSha256(bytesOf(secret), {bytesOf(label), serverBytes, clientBytes}, out);
}

}  // namespace

// ------------------------------------------------------------------------------ frames

FrameSplitter::FrameSplitter(std::span<const std::uint8_t> envelope, std::uint8_t messageId, std::size_t maxFrameBytes)
    : envelope_(envelope),
      messageId_(messageId),
      payloadBytes_(maxFrameBytes > kFrameHeaderBytes ? maxFrameBytes - kFrameHeaderBytes : 0) {}

bool FrameSplitter::next(std::span<std::uint8_t> out, std::size_t& length) {
  // As the dashboard's splitIntoFrames(): an empty envelope still makes one FIRST|LAST frame.
  if (payloadBytes_ == 0 || (started_ && offset_ >= envelope_.size())) {
    return false;
  }
  const std::size_t chunk = std::min(payloadBytes_, envelope_.size() - offset_);
  if (out.size() < kFrameHeaderBytes + chunk) {
    return false;
  }
  const bool isFirst = !started_;
  const bool isLast = offset_ + payloadBytes_ >= envelope_.size();
  out[0] = static_cast<std::uint8_t>((isFirst ? kFrameFirst : 0) | (isLast ? kFrameLast : 0));
  out[1] = messageId_;
  if (chunk > 0) {
    std::memcpy(out.data() + kFrameHeaderBytes, envelope_.data() + offset_, chunk);
  }
  length = kFrameHeaderBytes + chunk;
  offset_ += chunk;
  started_ = true;
  if (isLast) {
    offset_ = envelope_.size();
  }
  return true;
}

FrameAssembler::Result FrameAssembler::push(std::span<const std::uint8_t> frame) {
  Result result;
  if (frame.size() < kFrameHeaderBytes) {
    result.problem = "runt frame";
    return result;
  }
  const std::uint8_t flags = frame[0];
  const std::uint8_t messageId = frame[1];
  const std::span<const std::uint8_t> payload = frame.subspan(kFrameHeaderBytes);
  if (flags & kFrameFirst) {
    if (messageId_) {
      result.problem = "a message was abandoned part-way";
    }
    length_ = 0;
    messageId_ = messageId;
  } else if (messageId_ != messageId) {
    reset();
    result.problem = "a continuation frame for a message that never started";
    return result;
  }
  if (length_ + payload.size() > buffer_.size()) {
    reset();
    result.problem = "a message too large to buffer";
    return result;
  }
  if (!payload.empty()) {
    std::memcpy(buffer_.data() + length_, payload.data(), payload.size());
  }
  length_ += payload.size();
  if (!(flags & kFrameLast)) {
    return result;
  }
  result.envelope = std::span<const std::uint8_t>(buffer_.data(), length_);
  messageId_.reset();
  length_ = 0;
  return result;
}

void FrameAssembler::reset() {
  length_ = 0;
  messageId_.reset();
}

// ---------------------------------------------------------------------------- messages

std::size_t encodeMessage(MessageKind kind, std::string_view headerJson, std::span<const std::uint8_t> body,
                          std::span<std::uint8_t> out) {
  // The dashboard writes `{}` for a missing header.
  const std::string_view header = headerJson.empty() ? std::string_view("{}") : headerJson;
  const std::size_t length = kMessageHeaderBytes + header.size() + body.size();
  if (out.size() < length) {
    return 0;
  }
  out[0] = static_cast<std::uint8_t>(kind);
  putBigEndian32(out.data() + 1, static_cast<std::uint32_t>(header.size()));
  std::memcpy(out.data() + kMessageHeaderBytes, header.data(), header.size());
  if (!body.empty()) {
    std::memcpy(out.data() + kMessageHeaderBytes + header.size(), body.data(), body.size());
  }
  return length;
}

std::optional<Message> decodeMessage(std::span<const std::uint8_t> message) {
  if (message.size() < kMessageHeaderBytes) {
    return std::nullopt;
  }
  const std::uint32_t headerLength = getBigEndian32(message.data() + 1);
  if (headerLength > message.size() - kMessageHeaderBytes) {
    return std::nullopt;
  }
  return Message{message[0],
                 std::string_view(reinterpret_cast<const char*>(message.data() + kMessageHeaderBytes), headerLength),
                 message.subspan(kMessageHeaderBytes + headerLength)};
}

// --------------------------------------------------------------------------- envelopes

std::size_t clearEnvelope(std::span<const std::uint8_t> message, std::span<std::uint8_t> out) {
  if (out.size() < 1 + message.size()) {
    return 0;
  }
  out[0] = kEnvelopeClear;
  if (!message.empty()) {
    std::memcpy(out.data() + 1, message.data(), message.size());
  }
  return 1 + message.size();
}

std::optional<Envelope> parseEnvelope(std::span<const std::uint8_t> envelope) {
  if (envelope.empty()) {
    return std::nullopt;
  }
  if (envelope[0] == kEnvelopeClear) {
    return Envelope{false, 0, envelope.subspan(1)};
  }
  if (envelope[0] != kEnvelopeSealed || envelope.size() < kSealedPrefixBytes) {
    return std::nullopt;
  }
  const std::uint64_t counter =
      (std::uint64_t{getBigEndian32(envelope.data() + 1)} << 32) | getBigEndian32(envelope.data() + 5);
  return Envelope{true, counter, envelope.subspan(kSealedPrefixBytes)};
}

std::size_t EnvelopeSealer::seal(const AesKey& key, Direction direction, std::uint64_t counter,
                                 std::span<const std::uint8_t> message, std::span<std::uint8_t> out) {
  const std::size_t compressedLength = deflater_.compress(message, compressed_);
  if (compressedLength == 0 || out.size() < kSealedPrefixBytes + compressedLength + kGcmTagBytes) {
    return 0;
  }
  sealedPrefix(counter, out.data());
  const std::span<const std::uint8_t> prefix(out.data(), kSealedPrefixBytes);
  if (!aesGcmSeal(key, ivFor(direction, counter), prefix,
                  std::span<const std::uint8_t>(compressed_.data(), compressedLength),
                  out.subspan(kSealedPrefixBytes, compressedLength + kGcmTagBytes))) {
    return 0;
  }
  return kSealedPrefixBytes + compressedLength + kGcmTagBytes;
}

std::optional<std::size_t> EnvelopeSealer::open(const AesKey& key, Direction direction, std::uint64_t counter,
                                                std::span<const std::uint8_t> ciphertext,
                                                std::span<std::uint8_t> out) {
  if (ciphertext.size() < kGcmTagBytes || ciphertext.size() - kGcmTagBytes > compressed_.size()) {
    return std::nullopt;
  }
  std::uint8_t prefix[kSealedPrefixBytes];
  sealedPrefix(counter, prefix);
  const std::size_t compressedLength = ciphertext.size() - kGcmTagBytes;
  if (!aesGcmOpen(key, ivFor(direction, counter), prefix, ciphertext,
                  std::span<std::uint8_t>(compressed_.data(), compressedLength))) {
    return std::nullopt;
  }
  return inflateRaw(std::span<const std::uint8_t>(compressed_.data(), compressedLength), out);
}

// -------------------------------------------------------------------------------- keys

HexNonce randomNonce() {
  std::uint8_t bytes[16];
  fillRandom(bytes);
  HexNonce nonce = {};
  toHex(bytes, nonce.data());
  return nonce;
}

bool helloProof(std::string_view secret, std::string_view serverNonceHex, std::string_view clientNonceHex,
                HexDigest& out) {
  Sha256Digest digest;
  if (!labelledHmac(secret, "emcool/hello", serverNonceHex, clientNonceHex, digest)) {
    return false;
  }
  toHex(digest, out.data());
  return true;
}

bool deriveSessionKey(std::string_view secret, std::string_view serverNonceHex, std::string_view clientNonceHex,
                      AesKey& out) {
  Sha256Digest digest;
  if (!labelledHmac(secret, "emcool/session", serverNonceHex, clientNonceHex, digest)) {
    return false;
  }
  std::copy(digest.begin(), digest.end(), out.begin());
  return true;
}

bool proofsMatch(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  unsigned difference = 0;
  for (std::size_t index = 0; index < left.size(); index++) {
    difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
  }
  return difference == 0;
}

void toHex(std::span<const std::uint8_t> bytes, char* out) {
  constexpr char kDigits[] = "0123456789abcdef";
  for (std::size_t index = 0; index < bytes.size(); index++) {
    out[2 * index] = kDigits[bytes[index] >> 4];
    out[2 * index + 1] = kDigits[bytes[index] & 0x0f];
  }
  out[2 * bytes.size()] = '\0';
}

bool fromHex(std::string_view hex, std::span<std::uint8_t> out) {
  if (hex.size() != 2 * out.size()) return false;
  for (std::size_t index = 0; index < out.size(); index++) {
    unsigned value = 0;
    for (int half = 0; half < 2; half++) {
      const char digit = hex[2 * index + half];
      value <<= 4;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<unsigned>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<unsigned>(digit - 'a' + 10);
      } else {
        return false;
      }
    }
    out[index] = static_cast<std::uint8_t>(value);
  }
  return true;
}

}  // namespace emcool::ble
