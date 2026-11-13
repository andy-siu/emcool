#pragma once

// The protocol between the phone dashboard and the bike, byte for byte as dashboard/lib/ble-wire.js
// speaks it, so the phone side (the iOS app, or the GitHub Pages dashboard) needs no change.
// Layers, outermost first:
//
//   frame     [flags: FIRST=1 | LAST=2][messageId u8][payload…], one per GATT value
//   envelope  [0][message] clear, or [1][counter u64 BE][AES-256-GCM(deflate-raw(message))]
//   message   [kind u8][header length u32 BE][header JSON][body bytes]
//
// Unlike the JavaScript, nothing here allocates: every layer writes into a buffer the caller
// owns, sized for the largest message the link carries.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "emcool/ble/crypto.hpp"
#include "emcool/ble/deflate.hpp"

namespace emcool::ble {

inline constexpr int kProtocolVersion = 2;

inline constexpr std::string_view kServiceUuid = "7c0e0001-5f3a-4c8e-9d2b-c00e7a000001";
// Phone → bike, written with response.
inline constexpr std::string_view kRxCharacteristicUuid = "7c0e0002-5f3a-4c8e-9d2b-c00e7a000001";
// Bike → phone, by notification; values are at most min(MTU − 5, 512) bytes, as the reference sends them.
inline constexpr std::string_view kTxCharacteristicUuid = "7c0e0003-5f3a-4c8e-9d2b-c00e7a000001";
// Read-only `{ version, serverNonce }`.
inline constexpr std::string_view kInfoCharacteristicUuid = "7c0e0004-5f3a-4c8e-9d2b-c00e7a000001";

// The ATT ceiling on a characteristic value, and so on a phone → bike frame.
inline constexpr std::size_t kMaxWriteFrameBytes = 512;
// The largest message either way: snapshot chunks, patches and responses are built to fit. Every
// buffer on the link is sized by it (five of them, plus three outbound slots), so it is small to
// leave the heap room for OBD, the card and Wi-Fi: a 2 KB response piece and its header fit.
inline constexpr std::size_t kMaxMessageBytes = 2560;
// Sealed: 9-byte prefix, deflate output (at most 5 bytes over its input), GCM tag.
inline constexpr std::size_t kMaxEnvelopeBytes = 9 + kMaxMessageBytes + 5 + kGcmTagBytes;
inline constexpr std::size_t kFrameHeaderBytes = 2;

enum class MessageKind : std::uint8_t {
  // phone → bike
  kHello = 1,
  kRequest = 2,
  kSubscribe = 3,
  kUnsubscribe = 4,
  // bike → phone
  kRefused = 16,
  kWelcome = 17,
  kResponseHead = 18,
  kResponseBody = 19,
  kResponseEnd = 20,
  kStream = 21,
};

// In the IV, so the two directions never share one. The dashboard's TO_PI is this end.
enum class Direction : std::uint8_t { kToBike = 1, kToPhone = 2 };

// ------------------------------------------------------------------------------ frames

// Walks an envelope out as frames of at most `maxFrameBytes`. `messageId` only has to
// differ from its neighbour's, so a lost LAST frame shows up as the next message's FIRST.
class FrameSplitter {
 public:
  FrameSplitter(std::span<const std::uint8_t> envelope, std::uint8_t messageId, std::size_t maxFrameBytes);
  // Writes the next frame into `out`; false when there are no more.
  bool next(std::span<std::uint8_t> out, std::size_t& length);
  bool done() const { return started_ && offset_ >= envelope_.size(); }

 private:
  std::span<const std::uint8_t> envelope_;
  std::uint8_t messageId_;
  std::size_t payloadBytes_;
  std::size_t offset_ = 0;
  bool started_ = false;
};

// Glues frames back into envelopes. ATT delivers a link's values in order, so the only
// damage to detect is a message cut short, which push() reports rather than mends.
class FrameAssembler {
 public:
  struct Result {
    // Set when a LAST frame completed one; valid until the next push().
    std::optional<std::span<const std::uint8_t>> envelope;
    const char* problem = nullptr;
  };

  Result push(std::span<const std::uint8_t> frame);
  void reset();

 private:
  std::array<std::uint8_t, kMaxEnvelopeBytes> buffer_ = {};
  std::size_t length_ = 0;
  std::optional<std::uint8_t> messageId_;
};

// ---------------------------------------------------------------------------- messages

struct Message {
  std::uint8_t kind;
  std::string_view header;
  std::span<const std::uint8_t> body;
};

// The encoded length, or 0 if it doesn't fit in `out`.
std::size_t encodeMessage(MessageKind kind, std::string_view headerJson, std::span<const std::uint8_t> body,
                          std::span<std::uint8_t> out);
std::optional<Message> decodeMessage(std::span<const std::uint8_t> message);

// --------------------------------------------------------------------------- envelopes

struct Envelope {
  bool sealed;
  std::uint64_t counter;
  // The message, if clear; the ciphertext and tag, if sealed.
  std::span<const std::uint8_t> payload;
};

std::size_t clearEnvelope(std::span<const std::uint8_t> message, std::span<std::uint8_t> out);
std::optional<Envelope> parseEnvelope(std::span<const std::uint8_t> envelope);

// Seals and opens envelopes. Owns the deflate tables and the compressed-message buffer, so
// one of these per link, used from one task.
class EnvelopeSealer {
 public:
  std::size_t seal(const AesKey& key, Direction direction, std::uint64_t counter, std::span<const std::uint8_t> message,
                   std::span<std::uint8_t> out);
  // nullopt when the tag doesn't verify or the decrypted stream doesn't inflate into `out`.
  std::optional<std::size_t> open(const AesKey& key, Direction direction, std::uint64_t counter,
                                  std::span<const std::uint8_t> ciphertext, std::span<std::uint8_t> out);

 private:
  Deflater deflater_;
  std::array<std::uint8_t, kMaxMessageBytes + 5> compressed_ = {};
};

// -------------------------------------------------------------------------------- keys

// A 16-byte nonce as lower-case hex, NUL-terminated.
using HexNonce = std::array<char, 33>;
using HexDigest = std::array<char, 65>;

HexNonce randomNonce();

// HMAC-SHA256(secret, label ‖ serverNonce ‖ clientNonce) — the HELLO proof with label
// "emcool/hello", the session's AES key with "emcool/session". False when a nonce isn't
// lower-case hex.
bool helloProof(std::string_view secret, std::string_view serverNonceHex, std::string_view clientNonceHex,
                HexDigest& out);
bool deriveSessionKey(std::string_view secret, std::string_view serverNonceHex, std::string_view clientNonceHex,
                      AesKey& out);

// In time independent of where the two first differ.
bool proofsMatch(std::string_view left, std::string_view right);

void toHex(std::span<const std::uint8_t> bytes, char* out);
// Lower-case hex only, exactly out.size() bytes' worth.
bool fromHex(std::string_view hex, std::span<std::uint8_t> out);

}  // namespace emcool::ble
