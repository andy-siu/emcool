#pragma once

// The transport half of reading the VCU: put a request on the bus (segmented if it's long), answer the
// micro's flow control and ask for its consecutive frames, keep the 10 81 session alive (the micros
// close it after ~2.5 s idle, separately for A8 and A9), and give up on time. Every byte it sends is
// built by codec.hpp; it can't express a write.
//
// Coroutines on emcool::async's loop, so it reads like the original. One request at a time: a
// second while one is in flight is refused, never interleaved.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/async/task.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/snapshot.hpp"

namespace emcool::vcu {

// Puts one 8-byte frame on the bus; nullopt once queued, else why not.
using VcuSend = std::function<std::optional<std::string>(std::uint32_t id, const std::array<std::uint8_t, 8>& frame)>;
// The monotonic time, for arrival latency; and a frame's arrival stamp, if the driver gave one.
using ArrivalStamp = std::optional<double>;

enum class TransferStage : std::uint8_t { kRequestFlowControl, kFirstReply, kReplyTransfer };
const char* transferStageName(TransferStage stage);

struct MultiFrameResult {
  enum class Kind : std::uint8_t { kPayload, kTimeout, kAbandoned, kNotSent, kCancelled };
  Kind kind = Kind::kNotSent;
  std::vector<std::uint8_t> payload;
  bool sawFlowControlFromMicro = false;
  TransferStage stage = TransferStage::kFirstReply;
  std::string reason;
  std::optional<ArrivalLatency> flowControlLatency;
};

struct MultiFrameOptions {
  int maxPayloadBytes = 256;
  std::int64_t firstReplyTimeoutMs = 300;
  std::int64_t transferTimeoutMs = 400;
  std::int64_t requestFlowControlTimeoutMs = 150;
};

// One request and its reply, either direction possibly segmented.
class MultiFrameTransfer : public std::enable_shared_from_this<MultiFrameTransfer> {
 public:
  static std::shared_ptr<MultiFrameTransfer> start(Micro target, std::vector<std::uint8_t> requestPayload,
                                                   VcuSend send, MultiFrameOptions options);
  bool handleFrame(std::span<const std::uint8_t> data, ArrivalStamp arrival);
  void cancel(std::string reason);
  const async::Deferred<MultiFrameResult>& finished() const { return finished_; }

 private:
  MultiFrameTransfer(Micro target, std::vector<std::uint8_t> requestPayload, VcuSend send, MultiFrameOptions options);
  void begin();
  bool handleFlowControlFromMicro(const VcuFlowControl& flowControl);
  void onRequestFlowControlTimeout();
  void sendNextRequestFrame();
  bool transmit(const std::array<std::uint8_t, 8>& frame);
  void armTimer(std::int64_t ms, TransferStage stage);
  void settle(MultiFrameResult result);

  Micro target_;
  std::vector<std::uint8_t> requestPayload_;
  VcuSend send_;
  MultiFrameOptions options_;
  ExtendedIsoTpReassembler reassembler_;
  std::vector<std::array<std::uint8_t, 8>> outstanding_;
  int consecutiveFramesUntilNextFlowControl_ = 0;
  std::int64_t separationTimeMs_ = 0;
  bool sawFlowControlFromMicro_ = false;
  bool sawStrayFlowControl_ = false;
  std::optional<ArrivalLatency> flowControlLatency_;
  int framesHandled_ = 0;
  bool settled_ = false;
  async::TimerId timer_;
  async::TimerId pacer_;
  async::Deferred<MultiFrameResult> finished_;
};

// A one-off read: any target, bank and index.
struct VcuProbeOutcome : VcuReadOutcome {};

// How a multi-frame request came out.
struct VcuMultiFrameOutcome {
  enum class Status : std::uint8_t { kReply, kNoResponse, kAbandoned, kNoSession, kCancelled, kNotSent };
  Status status = Status::kNotSent;
  VcuMultiFrameReply reply;
  std::vector<std::uint8_t> payload;
  bool sawFlowControlFromMicro = false;
  std::optional<ArrivalLatency> flowControlLatency;
  TransferStage stage = TransferStage::kFirstReply;
  std::string reason;
};
const char* multiFrameStatusName(VcuMultiFrameOutcome::Status status);

class VcuKwpClient {
 public:
  static constexpr std::int64_t kSessionIdleLimitMs = 1500;
  static constexpr std::int64_t kResponseTimeoutMs = 300;
  static constexpr std::int64_t kPaceMs = 10;
  static constexpr int kReadMaxPayloadBytes = 32;

  explicit VcuKwpClient(VcuSend send);

  // A frame on the response id; true if it was this client's.
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival);
  async::Task<bool> openSession(Micro micro);
  // A session, then `3E`.
  async::Task<bool> ping(Micro micro);
  async::Task<VcuReadOutcome> readParameter(Micro micro, int index);
  async::Task<VcuReadOutcome> probe(Micro target, int bank, int index);
  async::Task<VcuMultiFrameOutcome> multiFrameRead(Micro target, VcuMultiFrameRequest request,
                                                   MultiFrameOptions options = {});
  bool cancelMultiFrameRead(std::string reason);
  void stop();

 private:
  struct State;
  struct Exchange {
    enum class Kind : std::uint8_t { kReply, kTimeout, kNotSent };
    Kind kind;
    VcuFrame frame;
    std::string reason;
  };
  struct SessionAttempt {
    bool opened;
    std::optional<std::string> notSent;
  };

  static async::Task<VcuReadResult> performRead(std::shared_ptr<State> state, Micro micro, int bank, int index);
  static async::Task<SessionAttempt> openSessionAttempt(std::shared_ptr<State> state, Micro micro);
  static async::Task<SessionAttempt> ensureSession(std::shared_ptr<State> state, Micro micro);
  static async::Task<Exchange> exchange(std::shared_ptr<State> state, Micro micro, VcuRequest request);
  static async::Task<MultiFrameResult> runTransfer(std::shared_ptr<State> state, Micro micro,
                                                   std::vector<std::uint8_t> payload, MultiFrameOptions options);
  static std::optional<std::string> busyReason(State& state);

  std::shared_ptr<State> state_;
};

}  // namespace emcool::vcu
