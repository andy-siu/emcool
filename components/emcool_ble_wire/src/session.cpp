#include "emcool/ble/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "emcool/ble/json_reader.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::ble {

using text::TextBuffer;

namespace {

bool isLowerHex(char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); }

}  // namespace

BleSession::BleSession(std::string_view secret) : serverNonce_(randomNonce()) {
  if (secretIsUsable(secret)) {
    std::memcpy(secret_.data(), secret.data(), secret.size());
    secretLength_ = secret.size();
  }
}

// The dashboard's validBleSecret() and the BLE_SECRET setting's rule: a generated key, 32 random bytes
// as 64 lower-case hex characters, or a typed one of 8-64 printable ASCII characters with at least one
// letter and one digit and no spaces, quotes or backslashes. ASCII, so the HMAC key is the same bytes
// as the phone's TextEncoder makes.
bool BleSession::secretIsUsable(std::string_view secret) {
  if (secret.size() == kSecretLength &&
      std::all_of(secret.begin(), secret.end(), [](char character) { return isLowerHex(character); })) {
    return true;
  }
  const auto has = [&](auto test) { return std::any_of(secret.begin(), secret.end(), test); };
  return secret.size() >= kMinimumSecretLength && secret.size() <= kSecretLength &&
         std::all_of(secret.begin(), secret.end(),
                     [](char character) {
                       return character > ' ' && character <= '~' && character != '"' && character != '\\';
                     }) &&
         has([](char character) {
           return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
         }) &&
         has([](char character) { return character >= '0' && character <= '9'; });
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

BleSession::Received BleSession::receive(std::span<const std::uint8_t> envelope) {
  Received result;
  const std::optional<Envelope> parsed = parseEnvelope(envelope);
  if (!parsed) {
    result.problem = "unreadable envelope";
    return result;
  }
  // A clear HELLO only counts whole in one frame, through receiveHello(); one assembled from
  // several would skip that path's checks.
  if (!parsed->sealed) {
    result.problem = "a clear message outside the single-frame HELLO path";
    return result;
  }
  if (!active_) {
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

BleSession::Received BleSession::receiveHello(std::span<const std::uint8_t> envelope, std::span<std::uint8_t> reply) {
  Received result;
  const std::optional<Envelope> parsed = parseEnvelope(envelope);
  if (!parsed || parsed->sealed) {
    result.problem = "unreadable HELLO";
    return result;
  }
  const std::optional<Message> decoded = decodeMessage(parsed->payload);
  if (!decoded) {
    result.problem = "undecodable HELLO";
    return result;
  }
  if (decoded->kind != static_cast<std::uint8_t>(MessageKind::kHello)) {
    result.problem = "only HELLO may be sent clear";
    return result;
  }
  char scratch[512];
  JsonObjectReader hello;
  if (!hello.read(decoded->header, scratch)) {
    result.problem = "malformed HELLO header";
    return result;
  }
  // Without a well-formed nonce there is nothing to address a refusal to, so none is sent.
  const JsonMember* clientNonce = hello.find("clientNonce");
  if (clientNonce == nullptr || clientNonce->type != JsonType::kString || clientNonce->text.size() != 32 ||
      !std::all_of(clientNonce->text.begin(), clientNonce->text.end(),
                   [](char character) { return isLowerHex(character); })) {
    result.problem = "HELLO requires a 16-byte lowercase-hex nonce";
    return result;
  }
  const JsonMember* version = hello.find("version");
  if (version == nullptr || version->type != JsonType::kNumber || version->number != kProtocolVersion) {
    char reason[64];
    std::snprintf(reason, sizeof reason, "protocol unsupported; this bike speaks %d", kProtocolVersion);
    result.replyLength = refusal(clientNonce->text, reason, reply);
    result.problem = "HELLO with another protocol version";
    return result;
  }
  HexDigest expectedProof;
  const JsonMember* proof = hello.find("proof");
  if (secretLength_ == 0 ||
      !helloProof(std::string_view(secret_.data(), secretLength_), serverNonce_.data(), clientNonce->text,
                  expectedProof) ||
      proof == nullptr || proof->type != JsonType::kString ||
      !proofsMatch(proof->text, std::string_view(expectedProof.data(), 64))) {
    result.replyLength = refusal(clientNonce->text, "wrong secret", reply);
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

std::size_t BleSession::refusal(std::string_view clientNonce, std::string_view reason, std::span<std::uint8_t> reply) {
  char header[160];
  TextBuffer text(header, sizeof header);
  text.append("{\"reason\":");
  text.appendJsonString(reason);
  text.append(",\"clientNonce\":");
  text.appendJsonString(clientNonce);
  text.append("}");
  if (text.overflowed()) return 0;
  const std::size_t messageLength =
      encodeMessage(MessageKind::kRefused, std::string_view(header, text.length()), {}, outgoing_);
  if (messageLength == 0) return 0;
  return clearEnvelope(std::span<const std::uint8_t>(outgoing_.data(), messageLength), reply);
}

}  // namespace emcool::ble
