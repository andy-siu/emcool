#pragma once

// One phone's authenticated session over the link. The handshake:
//
//   1. The phone reads `info`: { version, serverNonce }.
//   2. It sends a clear HELLO { version, clientNonce, proof }, where
//      proof = HMAC-SHA256(secret, "emcool/hello" ‖ serverNonce ‖ clientNonce).
//   3. A matching proof (compared in constant time) starts a session keyed by
//      HMAC-SHA256(secret, "emcool/session" ‖ …) and is answered with a sealed WELCOME, which
//      is the bike proving it knows the secret too. The server nonce then rotates, so a
//      recorded HELLO is useless.
//
// Every later envelope is sealed and counted; a counter not above the last one accepted is
// dropped. A wrong secret arms a 2 s cooldown that refuses every HELLO unchecked, so a short
// secret can't be guessed at radio speed. The nonce rotates only on success: rotating on
// failure would let anyone knock the real phone's HELLO off its nonce with garbage first.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "emcool/ble/wire.hpp"

namespace emcool::ble {

class BleSession {
 public:
  static constexpr std::int64_t kHelloFailureCooldownMs = 2000;
  static constexpr std::size_t kMinSecretLength = 12;
  static constexpr std::size_t kMaxSecretLength = 128;

  // Validate the secret with secretIsUsable() first; an unusable one makes every proof fail.
  explicit BleSession(std::string_view secret);
  static bool secretIsUsable(std::string_view secret);

  bool authenticated() const { return active_.has_value(); }
  // Changes whenever a session starts or ends, so work begun for one session can tell.
  std::uint32_t generation() const { return generation_; }
  void end();

  // `{"version":1,"serverNonce":"…"}`, the info characteristic's value.
  std::size_t info(std::span<char> out) const;

  struct Received {
    // A sealed message that opened. Points into this session's buffer: valid until the next receive().
    std::optional<Message> message;
    // An envelope to send back — REFUSED (clear) or WELCOME (sealed) — of this length, in `reply`.
    std::size_t replyLength = 0;
    // Why nothing came of the envelope, for the log.
    const char* problem = nullptr;
    bool startedSession = false;
  };

  Received receive(std::span<const std::uint8_t> envelope, std::int64_t nowMs, std::span<std::uint8_t> reply);

  // Seals a message to the phone into `out`; 0 if there's no session or it won't fit.
  std::size_t seal(MessageKind kind, std::string_view headerJson, std::span<const std::uint8_t> body,
                   std::span<std::uint8_t> out);

 private:
  struct ActiveSession {
    AesKey key;
    std::optional<std::uint64_t> lastInboundCounter;
    std::uint64_t nextOutboundCounter = 0;
  };

  Received receiveHello(std::span<const std::uint8_t> message, std::int64_t nowMs, std::span<std::uint8_t> reply);
  std::size_t refusal(std::string_view reason, std::span<std::uint8_t> reply);

  std::array<char, kMaxSecretLength> secret_ = {};
  std::size_t secretLength_ = 0;
  HexNonce serverNonce_;
  std::optional<ActiveSession> active_;
  std::optional<std::int64_t> lastHelloFailureMs_;
  std::uint32_t generation_ = 0;
  EnvelopeSealer sealer_;
  std::array<std::uint8_t, kMaxMessageBytes> plaintext_ = {};
  std::array<std::uint8_t, kMaxMessageBytes> outgoing_ = {};
};

}  // namespace emcool::ble
