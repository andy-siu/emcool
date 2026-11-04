// The phone link's wire protocol, piece by piece: crypto against published vectors, the
// deflate encoder against zlib, the JSON reader against good and hostile input, framing,
// envelopes, and the session handshake played from the phone's side. tools/check-ble.mts then
// does it all again against the dashboard's own ble-wire.js — the code the phone runs.

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/ble/crypto.hpp"
#include "emcool/ble/deflate.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/ble/session.hpp"
#include "emcool/ble/wire.hpp"

using namespace emcool::ble;

namespace {

std::span<const std::uint8_t> bytesOf(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string jsonLike(std::mt19937& random, std::size_t length) {
  static const char* keys[] = {"soc", "pack_v", "pack_a", "lmu3_cell7_mv", "speed_can_kmh", "inverter_igbt_c"};
  static const char* units[] = {"%", "V", "A", "mV", "km/h", "°C"};
  std::string text = "{\"type\":\"snapshot\",\"ts\":1767225600123,\"signals\":{";
  while (text.size() < length) {
    const auto index = random() % 6;
    text += "\"" + std::string(keys[index]) + "\":{\"value\":" + std::to_string(random() % 5000) + ".5,\"unit\":\"" +
            units[index] + "\",\"group\":\"battery\",\"ts\":17672256" + std::to_string(10000 + random() % 89999) + "},";
  }
  text.resize(length);
  return text;
}

void checkCrypto();
void checkDeflate();
void checkJson();
void checkFrames();
void checkEnvelopes();
void checkSession();

}  // namespace

int main() {
  EMCOOL_CHECK(initCrypto(), "crypto did not initialise");
  checkCrypto();
  checkDeflate();
  checkJson();
  checkFrames();
  checkEnvelopes();
  checkSession();
  std::printf("check_ble_wire: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}

namespace {

void checkCrypto() { EMCOOL_CHECK(cryptoSelfTest(), "the crypto self-test failed"); }

void checkRoundTrip(Deflater& deflater, std::span<const std::uint8_t> input, const char* what) {
  std::vector<std::uint8_t> compressed(input.size() + 5);
  const std::size_t length = deflater.compress(input, compressed);
  EMCOOL_CHECK(length > 0 && length <= input.size() + 5, "%s (%zu bytes): compressed to %zu", what, input.size(),
               length);
  // zlib, independently of inflateRaw().
  std::vector<std::uint8_t> restored(input.size() + 1);
  z_stream stream = {};
  inflateInit2(&stream, -15);
  stream.next_in = compressed.data();
  stream.avail_in = static_cast<uInt>(length);
  stream.next_out = restored.data();
  stream.avail_out = static_cast<uInt>(restored.size());
  const int result = inflate(&stream, Z_FINISH);
  const std::size_t produced = stream.total_out;
  inflateEnd(&stream);
  EMCOOL_CHECK(result == Z_STREAM_END && produced == input.size() &&
                   std::memcmp(restored.data(), input.data(), input.size()) == 0,
               "%s (%zu bytes): zlib did not get the input back (result %d, %zu bytes)", what, input.size(), result,
               produced);
  const std::optional<std::size_t> viaWrapper =
      inflateRaw(std::span<const std::uint8_t>(compressed.data(), length), restored);
  EMCOOL_CHECK(viaWrapper && *viaWrapper == input.size(), "%s: inflateRaw disagreed with zlib", what);
}

void checkDeflate() {
  Deflater deflater;
  std::mt19937 random(7);
  checkRoundTrip(deflater, {}, "empty");
  for (std::size_t size : {1u, 2u, 3u, 4u, 257u, 258u, 259u, 4095u, 4096u, 4097u, 4608u}) {
    std::vector<std::uint8_t> uniform(size);
    for (auto& byte : uniform) byte = static_cast<std::uint8_t>(random());
    checkRoundTrip(deflater, uniform, "random bytes");
    std::vector<std::uint8_t> run(size, 'a');
    checkRoundTrip(deflater, run, "one repeated byte");
  }
  for (int trial = 0; trial < 300; trial++) {
    const std::size_t size = random() % (kMaxMessageBytes + 1);
    const std::string text = jsonLike(random, size);
    checkRoundTrip(deflater, bytesOf(text), "telemetry-like JSON");
    // Short alphabets make long matches at every distance up to the window.
    std::vector<std::uint8_t> sparse(size);
    for (auto& byte : sparse) byte = static_cast<std::uint8_t>('a' + random() % 3);
    checkRoundTrip(deflater, sparse, "three-letter text");
  }
  const std::string sample = jsonLike(random, kMaxMessageBytes);
  std::vector<std::uint8_t> compressed(sample.size() + 5);
  const std::size_t length = deflater.compress(bytesOf(sample), compressed);
  EMCOOL_CHECK(length * 2 < sample.size(), "telemetry JSON only compressed %zu → %zu", sample.size(), length);
  std::uint8_t tooSmall[8];
  EMCOOL_CHECK(deflater.compress(bytesOf(sample), tooSmall) == 0, "compressing into 8 bytes claimed success");

  std::uint8_t output[64];
  const std::uint8_t garbage[] = {0xff, 0xfe, 0x00, 0x13, 0x37};
  EMCOOL_CHECK(!inflateRaw(garbage, output), "garbage inflated");
  const std::size_t sampleLength = deflater.compress(bytesOf(sample), compressed);
  EMCOOL_CHECK(!inflateRaw(std::span<const std::uint8_t>(compressed.data(), sampleLength), output),
               "inflating 4.6 KB into 64 bytes claimed success");
}

void checkJson() {
  char scratch[256];
  JsonObjectReader reader;
  EMCOOL_CHECK(reader.read(R"({"version":1,"clientNonce":"00ff","proof":"ab"})", scratch), "a plain HELLO failed");
  EMCOOL_CHECK(reader.find("version") && reader.find("version")->number == 1, "version");
  EMCOOL_CHECK(reader.find("clientNonce") && reader.find("clientNonce")->text == "00ff", "clientNonce");
  EMCOOL_CHECK(
      reader.read(R"( { "id" : 7, "method":"GET", "path":"/status", "headers":{"x-a":"b","n":[1,2,{"c":null}]} } )",
                  scratch),
      "a REQUEST with nested headers failed");
  EMCOOL_CHECK(reader.find("path") && reader.find("path")->text == "/status", "path");
  EMCOOL_CHECK(reader.find("headers") && reader.find("headers")->type == JsonType::kObject, "headers type");
  EMCOOL_CHECK(reader.read(R"({"s":"a\"b\\c\/d\né😀"})", scratch), "escapes failed");
  EMCOOL_CHECK(reader.find("s") && reader.find("s")->text == "a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80", "unescaping");
  EMCOOL_CHECK(reader.read(R"({"n":-0.5e+3,"t":true,"f":false,"z":null})", scratch), "literals failed");
  EMCOOL_CHECK(reader.find("n") && reader.find("n")->number == -500, "number");
  EMCOOL_CHECK(reader.read("{}", scratch) && reader.members().empty(), "an empty object");

  const char* invalid[] = {
      "",          "[]",          "{",          R"({"a":1,})",       R"({a:1})",          R"({"a":01})",
      R"({"a":1.})", R"({"a":.5})", R"({"a":"\x"})", R"({"a":"\ud800"})", R"({"a":1} x)", R"({"a":tru})",
      "{\"a\":\"\x01\"}", R"({"a":"unterminated)", R"({"a":1 "b":2})", R"({"a":-})",
  };
  for (const char* text : invalid) {
    EMCOOL_CHECK(!reader.read(text, scratch), "accepted invalid JSON: %s", text);
  }
  std::string deep = "{\"a\":";
  for (int level = 0; level < 40; level++) deep += "[";
  for (int level = 0; level < 40; level++) deep += "]";
  deep += "}";
  EMCOOL_CHECK(!reader.read(deep, scratch), "accepted nesting 40 deep");
  const std::string longString = "{\"a\":\"" + std::string(1000, 'x') + "\"}";
  EMCOOL_CHECK(!reader.read(longString, scratch), "a string past the scratch buffer was accepted");
}

void checkFrames() {
  std::mt19937 random(11);
  FrameAssembler assembler;
  for (std::size_t maxFrame : {23u - 3u, 180u, 182u, 244u, 512u}) {
    for (std::size_t size : {0u, 1u, 177u, 178u, 179u, 1000u, 4000u}) {
      std::vector<std::uint8_t> envelope(size);
      for (auto& byte : envelope) byte = static_cast<std::uint8_t>(random());
      FrameSplitter splitter(envelope, static_cast<std::uint8_t>(size), maxFrame);
      std::uint8_t frame[kMaxWriteFrameBytes];
      std::size_t length = 0;
      std::size_t frames = 0;
      std::optional<std::vector<std::uint8_t>> assembled;
      while (splitter.next(frame, length)) {
        frames++;
        EMCOOL_CHECK(length <= maxFrame, "a %zu-byte frame for a %zu limit", length, maxFrame);
        const auto result = assembler.push(std::span<const std::uint8_t>(frame, length));
        EMCOOL_CHECK(result.problem == nullptr, "assembly problem: %s", result.problem);
        if (result.envelope) assembled.emplace(result.envelope->begin(), result.envelope->end());
      }
      const std::size_t expectedFrames = size == 0 ? 1 : (size + maxFrame - 3) / (maxFrame - 2);
      EMCOOL_CHECK(frames == expectedFrames, "%zu bytes at %zu: %zu frames, expected %zu", size, maxFrame, frames,
                   expectedFrames);
      EMCOOL_CHECK(assembled && *assembled == envelope, "%zu bytes at %zu did not reassemble", size, maxFrame);
    }
  }
  // A FIRST arriving mid-message abandons the old one; a stray continuation is refused.
  const std::uint8_t first[] = {1, 5, 'a'};
  const std::uint8_t nextFirst[] = {3, 6, 'b'};
  const std::uint8_t stray[] = {2, 9, 'c'};
  assembler.push(first);
  const auto abandoned = assembler.push(nextFirst);
  EMCOOL_CHECK(abandoned.problem != nullptr && abandoned.envelope && abandoned.envelope->size() == 1,
               "an abandoned message was not reported, or the new one was lost");
  EMCOOL_CHECK(assembler.push(stray).problem != nullptr && !assembler.push(stray).envelope,
               "a stray continuation was accepted");
}

void checkEnvelopes() {
  EnvelopeSealer sealer;
  AesKey key;
  fillRandom(key);
  const std::string text = R"({"type":"patch","ts":1,"signals":{}})";
  std::uint8_t message[256];
  const std::size_t messageLength = encodeMessage(MessageKind::kStream, text, {}, message);
  std::uint8_t envelope[512];
  const std::size_t envelopeLength = sealer.seal(key, Direction::kToPhone, 42, std::span(message, messageLength), envelope);
  EMCOOL_CHECK(envelopeLength > 0, "seal failed");
  const auto parsed = parseEnvelope(std::span<const std::uint8_t>(envelope, envelopeLength));
  EMCOOL_CHECK(parsed && parsed->sealed && parsed->counter == 42, "the sealed envelope did not parse");
  std::uint8_t opened[256];
  const auto openedLength = sealer.open(key, Direction::kToPhone, 42, parsed->payload, opened);
  EMCOOL_CHECK(openedLength && *openedLength == messageLength && std::memcmp(opened, message, messageLength) == 0,
               "the envelope did not open back to the message");
  EMCOOL_CHECK(!sealer.open(key, Direction::kToBike, 42, parsed->payload, opened), "opened with the wrong direction");
  EMCOOL_CHECK(!sealer.open(key, Direction::kToPhone, 43, parsed->payload, opened), "opened with the wrong counter");
  envelope[envelopeLength - 1] ^= 0x80;
  const auto tampered = parseEnvelope(std::span<const std::uint8_t>(envelope, envelopeLength));
  EMCOOL_CHECK(!sealer.open(key, Direction::kToPhone, 42, tampered->payload, opened), "a tampered envelope opened");
  const auto decoded = decodeMessage(std::span<const std::uint8_t>(message, messageLength));
  EMCOOL_CHECK(decoded && decoded->kind == 21 && decoded->header == text && decoded->body.empty(), "decodeMessage");
}

// The phone's side of a handshake, with this file's own functions.
struct Phone {
  explicit Phone(std::string phoneSecret) : secret(std::move(phoneSecret)) {}
  std::string secret;
  AesKey key = {};
  std::uint64_t nextCounter = 0;
  EnvelopeSealer sealer;

  std::vector<std::uint8_t> hello(const BleSession& session, std::string_view clientNonce) {
    char info[128];
    const std::size_t infoLength = session.info(info);
    JsonObjectReader reader;
    char scratch[128];
    reader.read(std::string_view(info, infoLength), scratch);
    const std::string serverNonce(reader.find("serverNonce")->text);
    HexDigest proof;
    helloProof(secret, serverNonce, clientNonce, proof);
    deriveSessionKey(secret, serverNonce, clientNonce, key);
    nextCounter = 0;
    const std::string header =
        "{\"version\":1,\"clientNonce\":\"" + std::string(clientNonce) + "\",\"proof\":\"" + proof.data() + "\"}";
    std::uint8_t message[512];
    const std::size_t length = encodeMessage(MessageKind::kHello, header, {}, message);
    std::vector<std::uint8_t> envelope(length + 1);
    clearEnvelope(std::span(message, length), envelope);
    return envelope;
  }

  std::vector<std::uint8_t> sealed(MessageKind kind, std::string_view header) {
    std::uint8_t message[512];
    const std::size_t length = encodeMessage(kind, header, {}, message);
    std::vector<std::uint8_t> envelope(1024);
    envelope.resize(sealer.seal(key, Direction::kToBike, nextCounter++, std::span(message, length), envelope));
    return envelope;
  }

  std::optional<Message> open(std::span<const std::uint8_t> envelope, std::vector<std::uint8_t>& plaintext) {
    const auto parsed = parseEnvelope(envelope);
    if (!parsed) return std::nullopt;
    if (!parsed->sealed) return decodeMessage(parsed->payload);
    plaintext.resize(kMaxMessageBytes);
    const auto length = sealer.open(key, Direction::kToPhone, parsed->counter, parsed->payload, plaintext);
    if (!length) return std::nullopt;
    return decodeMessage(std::span<const std::uint8_t>(plaintext.data(), *length));
  }
};

std::string refusalReason(const std::optional<Message>& message) {
  if (!message || message->kind != static_cast<std::uint8_t>(MessageKind::kRefused)) return "(not a REFUSED)";
  JsonObjectReader reader;
  char scratch[256];
  reader.read(message->header, scratch);
  return reader.find("reason") ? std::string(reader.find("reason")->text) : "(no reason)";
}

void checkSession() {
  const std::string secret = "correct horse battery";
  EMCOOL_CHECK(!BleSession::secretIsUsable("too short"), "an 9-character secret was accepted");
  BleSession session(secret);
  std::vector<std::uint8_t> reply(kMaxEnvelopeBytes);
  std::vector<std::uint8_t> plaintext;

  Phone stranger("the wrong secret!!");
  auto result = session.receive(stranger.hello(session, "00112233445566778899aabbccddeeff"), 1000, reply);
  EMCOOL_CHECK(!result.startedSession && refusalReason(stranger.open(std::span(reply.data(), result.replyLength),
                                                                     plaintext)) == "wrong secret",
               "a wrong secret was not refused as one");
  Phone phone(secret);
  result = session.receive(phone.hello(session, "ffeeddccbbaa99887766554433221100"), 2500, reply);
  EMCOOL_CHECK(refusalReason(phone.open(std::span(reply.data(), result.replyLength), plaintext)) ==
                   "too soon after a failed attempt",
               "the cooldown did not hold");

  const auto recordedHello = phone.hello(session, "ffeeddccbbaa99887766554433221100");
  result = session.receive(recordedHello, 3100, reply);
  EMCOOL_CHECK(result.startedSession && session.authenticated(), "the right secret did not start a session");
  const auto welcome = phone.open(std::span(reply.data(), result.replyLength), plaintext);
  EMCOOL_CHECK(welcome && welcome->kind == static_cast<std::uint8_t>(MessageKind::kWelcome),
               "the WELCOME did not open with the phone's key");

  const auto subscribe = phone.sealed(MessageKind::kSubscribe, "{}");
  result = session.receive(subscribe, 3200, reply);
  EMCOOL_CHECK(result.message && result.message->kind == static_cast<std::uint8_t>(MessageKind::kSubscribe),
               "a sealed SUBSCRIBE did not come through");
  result = session.receive(subscribe, 3300, reply);
  EMCOOL_CHECK(!result.message && result.problem != nullptr, "a replayed envelope came through again");

  // The nonce rotated, so replaying the recorded HELLO is a wrong secret now.
  const std::uint32_t generation = session.generation();
  result = session.receive(recordedHello, 9000, reply);
  EMCOOL_CHECK(!result.startedSession && session.generation() == generation, "a recorded HELLO started a session");

  session.end();
  result = session.receive(phone.sealed(MessageKind::kSubscribe, "{}"), 12000, reply);
  EMCOOL_CHECK(refusalReason(phone.open(std::span(reply.data(), result.replyLength), plaintext)) ==
                   "no session — send HELLO first",
               "a sealed envelope with no session was not refused");
}

}  // namespace
