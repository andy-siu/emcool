#pragma once

// The bike's end of the phone link, ported from cool-eva src/ble-server/server.ts and
// stream.ts: frames in from the rx characteristic, a session per authenticated phone, the
// live telemetry stream, and requests answered by a RequestHandler. Transport-free: the
// firmware's NimBLE glue (and the host harness) deliver writes and pull notifications, and
// every method is called from one task.
//
// Outbound work is generated lazily, a slot at a time, as notifications drain: response
// pieces first, then snapshot chunks, then patches. So the radio's real pace — NimBLE has
// genuine flow control where cool-eva's BlueZ had to be paced by a guess — is what sets how
// fast the snapshot goes, and nothing is built that has nowhere to go. cool-eva drops patches
// while its queue is deep; here a slow link instead leaves changes marked in the store, and
// the next patch carries their latest values, so nothing is shed and nothing goes stale.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "emcool/ble/session.hpp"
#include "emcool/ble/wire.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/ble_server/telemetry_stream.hpp"
#include "emcool/signals/signal_store.hpp"

namespace emcool::ble_server {

struct BleServerCounters {
  std::uint32_t sessionsStarted = 0;
  std::uint32_t refusals = 0;
  std::uint32_t problems = 0;
  std::uint32_t requests = 0;
  std::uint32_t snapshots = 0;
  std::uint64_t framesSent = 0;
  std::uint64_t bytesSent = 0;
};

class BleServer {
 public:
  static constexpr std::int64_t kPatchWindowMs = 100;
  static constexpr std::int64_t kHeartbeatMs = 5000;
  static constexpr std::size_t kOutboundSlots = 3;
  static constexpr std::size_t kMaxQueuedRequests = 8;
  // The most of a request's body kept, held only while the request waits: a console command with a
  // 128-character password fits with every character escaped twice. A longer body is answered 413,
  // never cut short and acted on.
  static constexpr std::size_t kMaxRequestBodyBytes = 1024;
  // cool-eva sends response bodies in pieces of at most this much, each its own message.
  static constexpr std::size_t kResponseChunkBytes = 4 * 1024;
  static constexpr std::uint16_t kDefaultAttMtu = 23;
  static constexpr std::int64_t kRequestTimeoutMs = 180000;
  // cool-eva's Rejections: a summary log line and up to four clear refusals per window.
  static constexpr std::int64_t kRejectionWindowMs = 2000;
  static constexpr std::uint32_t kRefusalsPerWindow = 4;

  BleServer(std::string_view secret, signals::SignalStore& store, std::mutex& storeMutex, RequestHandler& requests);

  // The link: a phone connected, dropped, or turned tx notifications on or off.
  void connected();
  void disconnected();
  void notificationsEnabled(bool enabled);
  void mtuChanged(std::uint16_t mtu);

  void receiveFrame(std::span<const std::uint8_t> frame, std::int64_t nowMonotonicMs);
  std::size_t info(std::span<char> out) const { return session_.info(out); }

  // Generates what is due — response pieces, snapshot chunks, patches — into free slots.
  void tick(std::int64_t nowMonotonicMs, std::int64_t nowWallClockMs);

  // The next notification. Call frameSent() once the stack has taken it; if it didn't, the
  // same frame comes back next time.
  bool nextFrame(std::span<std::uint8_t> out, std::size_t& length);
  void frameSent();

  std::size_t queuedBytes() const;
  bool authenticated() const { return session_.authenticated(); }
  const BleServerCounters& counters() const { return counters_; }

 private:
  struct Slot {
    std::array<std::uint8_t, ble::kMaxEnvelopeBytes> bytes = {};
    std::size_t length = 0;
    std::uint8_t messageId = 0;
    // A response's message, not the stream's (nor a handshake reply).
    bool response = false;
  };

  struct PendingRequest {
    std::int64_t id;
    std::array<char, 8> method;
    std::array<char, 128> path;
    // The X-Cool-Eva header's value, which says what a state-changing request is for.
    std::array<char, 24> coolEva;
    std::string body;
    bool bodyTooLarge;
  };

  // kWaiting: the handler will answer later (a probe, say), and is asked each tick.
  enum class ResponseStage : std::uint8_t { kNone, kWaiting, kHead, kBody, kEnd };

  void receiveHello(std::span<const std::uint8_t> envelope, std::int64_t nowMonotonicMs);
  bool allowRefusal(std::int64_t nowMonotonicMs);
  void noteProblem(const char* reason, std::int64_t nowMonotonicMs);
  void flushProblems(std::int64_t nowMonotonicMs);
  void endSession(const char* reason);
  void dispatch(const ble::Message& message, std::int64_t nowMonotonicMs);
  void queueRequest(std::string_view header, std::span<const std::uint8_t> body);
  Slot* freeSlot();
  void commitSlot(Slot& slot, bool response = false);
  bool mayTakeSlot(bool response) const;
  bool sealIntoSlot(ble::MessageKind kind, std::string_view header, std::span<const std::uint8_t> body);
  void advanceResponse(std::int64_t nowMonotonicMs);
  void advanceStream(std::int64_t nowMonotonicMs, std::int64_t nowWallClockMs);

  ble::BleSession session_;
  ble::FrameAssembler assembler_;
  signals::SignalStore& store_;
  std::mutex& storeMutex_;
  RequestHandler& requests_;
  TelemetryStream stream_;
  bool streaming_ = false;
  std::int64_t lastPatchMs_ = 0;
  std::int64_t lastSnapshotMs_ = 0;
  std::uint16_t mtu_ = kDefaultAttMtu;

  std::array<Slot, kOutboundSlots> slots_;
  std::size_t slotHead_ = 0;
  std::size_t slotCount_ = 0;
  std::uint8_t nextMessageId_ = 0;
  std::optional<ble::FrameSplitter> splitter_;
  std::optional<ble::FrameSplitter> attempt_;
  std::size_t attemptLength_ = 0;

  std::array<PendingRequest, kMaxQueuedRequests> requestQueue_ = {};
  std::size_t requestCount_ = 0;
  ResponseStage responseStage_ = ResponseStage::kNone;
  std::int64_t responseId_ = 0;
  HttpResponse response_ = {};
  std::size_t responseOffset_ = 0;
  std::int64_t waitingSinceMs_ = 0;
  // The response is the server's own (a 400 for the path, a 413 for the body), not the handler's.
  std::string_view ownBody_;
  // How far the body fell short of its promised length; the END says so.
  std::size_t bodyShortBy_ = 0;
  std::array<char, ble::kMaxMessageBytes - 64> header_ = {};

  std::optional<std::int64_t> refusalWindowStartMs_;
  std::uint32_t refusalsInWindow_ = 0;
  std::optional<std::int64_t> lastProblemLogMs_;
  std::uint32_t pendingProblems_ = 0;
  const char* lastProblem_ = "";

  BleServerCounters counters_;
};

}  // namespace emcool::ble_server
