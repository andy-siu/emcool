#include "emcool/ble/session.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "emcool/ble/json_reader.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::ble {

using text::TextBuffer;

BleSession::BleSession(std::string_view secret) : serverNonce_(randomNonce()) {
  if (secretIsUsable(secret)) {
    std::memcpy(secret_.data(), secret.data(), secret.size());
    secretLength_ = secret.size();
  }
}

bool BleSession::secretIsUsable(std::string_view secret) {
  return secret.size() >= kMinSecretLength && secret.size() <= kMaxSecretLength;
}

void BleSession::end() {
  active_.reset();
  generation_++;
}

std::size_t BleSession::info(std::span<char> out) const {
  TextBuffer text(out.data(), out.size());
  text.append("{\"version\":");
  text.appendInteger(kProtocolVersion);
  text.append(",\"serverNonce\":");
  text.appendJsonString(serverNonce_.data());
  text.append("}");
  return text.overflowed() ? 0 : text.length();
}

BleSession::Received BleSession::receive(std::span<const std::uint8_t> envelope, std::int64_t nowMs,
                                         std::span<std::uint8_t> reply) {
  Received result;
  const std::optional<Envelope> parsed = parseEnvelope(envelope);
  if (!parsed) {
    result.problem = "unreadable envelope";
    return result;
  }
  if (!parsed->sealed) {
    return receiveHello(parsed->payload, nowMs, reply);
  }
  if (!active_) {
    result.replyLength = refusal("no session — send HELLO first", reply);
    result.problem = "sealed envelope with no session";
    return result;
  }
  if (active_->lastInboundCounter && parsed->counter <= *active_->lastInboundCounter) {
    result.problem = "replayed or reordered counter";
    return result;
  }
  const std::optional<std::size_t> opened =
      sealer_.open(active_->key, Direction::kToBike, parsed->counter, parsed->payload, plaintext_);
  if (!opened) {
    result.problem = "envelope did not open";
    return result;
  }
  active_->lastInboundCounter = parsed->counter;
  result.message = decodeMessage(std::span<const std::uint8_t>(plaintext_.data(), *opened));
  if (!result.message) {
    result.problem = "undecodable message";
  }
  return result;
}

std::size_t BleSession::seal(MessageKind kind, std::string_view headerJson, std::span<const std::uint8_t> body,
                             std::span<std::uint8_t> out) {
  if (!active_) return 0;
  const std::size_t messageLength = encodeMessage(kind, headerJson, body, outgoing_);
  if (messageLength == 0) return 0;
  const std::size_t length = sealer_.seal(active_->key, Direction::kToPhone, active_->nextOutboundCounter,
                                          std::span<const std::uint8_t>(outgoing_.data(), messageLength), out);
  if (length != 0) {
    active_->nextOutboundCounter++;
  }
  return length;
}

BleSession::Received BleSession::receiveHello(std::span<const std::uint8_t> message, std::int64_t nowMs,
                                              std::span<std::uint8_t> reply) {
  Received result;
  if (lastHelloFailureMs_ && nowMs - *lastHelloFailureMs_ < kHelloFailureCooldownMs) {
    result.replyLength = refusal("too soon after a failed attempt", reply);
    result.problem = "HELLO inside cooldown";
    return result;
  }
  const std::optional<Message> decoded = decodeMessage(message);
  if (!decoded) {
    result.problem = "undecodable HELLO";
    return result;
  }
  if (decoded->kind != static_cast<std::uint8_t>(MessageKind::kHello)) {
    result.replyLength = refusal("only HELLO may be sent clear", reply);
    result.problem = "a clear message that isn't HELLO";
    return result;
  }
  char scratch[512];
  JsonObjectReader hello;
  if (!hello.read(decoded->header, scratch)) {
    result.problem = "undecodable HELLO";
    return result;
  }
  const JsonMember* version = hello.find("version");
  if (version == nullptr || version->type != JsonType::kNumber || version->number != kProtocolVersion) {
    char reason[96];
    std::snprintf(reason, sizeof reason, "protocol %.*s unsupported; this bike speaks %d",
                  version ? static_cast<int>(version->text.size()) : 1, version ? version->text.data() : "?",
                  kProtocolVersion);
    result.replyLength = refusal(reason, reply);
    result.problem = "HELLO with another protocol version";
    return result;
  }
  const JsonMember* clientNonce = hello.find("clientNonce");
  HexDigest expectedProof;
  if (clientNonce == nullptr || clientNonce->type != JsonType::kString ||
      !helloProof(std::string_view(secret_.data(), secretLength_), serverNonce_.data(), clientNonce->text,
                  expectedProof)) {
    result.replyLength = refusal("malformed nonce", reply);
    result.problem = "HELLO with a malformed nonce";
    return result;
  }
  const JsonMember* proof = hello.find("proof");
  if (secretLength_ == 0 || proof == nullptr || proof->type != JsonType::kString ||
      !proofsMatch(proof->text, std::string_view(expectedProof.data(), 64))) {
    lastHelloFailureMs_ = nowMs;
    result.replyLength = refusal("wrong secret", reply);
    result.problem = "HELLO with a wrong secret";
    return result;
  }

  ActiveSession session;
  if (!deriveSessionKey(std::string_view(secret_.data(), secretLength_), serverNonce_.data(), clientNonce->text,
                        session.key)) {
    result.problem = "could not derive the session key";
    return result;
  }
  serverNonce_ = randomNonce();
  active_ = session;
  generation_++;
  char welcome[32];
  std::snprintf(welcome, sizeof welcome, "{\"version\":%d}", kProtocolVersion);
  result.replyLength = seal(MessageKind::kWelcome, welcome, {}, reply);
  result.startedSession = true;
  return result;
}

std::size_t BleSession::refusal(std::string_view reason, std::span<std::uint8_t> reply) {
  char header[160];
  TextBuffer text(header, sizeof header);
  text.append("{\"reason\":");
  text.appendJsonString(reason);
  text.append("}");
  if (text.overflowed()) return 0;
  const std::size_t messageLength =
      encodeMessage(MessageKind::kRefused, std::string_view(header, text.length()), {}, outgoing_);
  if (messageLength == 0) return 0;
  return clearEnvelope(std::span<const std::uint8_t>(outgoing_.data(), messageLength), reply);
}

}  // namespace emcool::ble
