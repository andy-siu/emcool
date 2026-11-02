#include "emcool/vcu/kwp_client.hpp"

#include <cstdio>

#include "emcool/log.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu";
constexpr int kFlowControlFrameAllowance = 16;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

double now() { return static_cast<double>(async::currentLoop().now()); }

int microSlot(Micro micro) { return micro == Micro::kA8 ? 0 : 1; }

}  // namespace

const char* transferStageName(TransferStage stage) {
  switch (stage) {
    case TransferStage::kRequestFlowControl:
      return "request-flow-control";
    case TransferStage::kFirstReply:
      return "first-reply";
    case TransferStage::kReplyTransfer:
      return "reply-transfer";
  }
  return "first-reply";
}

// --- One transfer -------------------------------------------------------------------------------

MultiFrameTransfer::MultiFrameTransfer(Micro target, std::vector<std::uint8_t> requestPayload, VcuSend send,
                                       MultiFrameOptions options)
    : target_(target),
      requestPayload_(std::move(requestPayload)),
      send_(std::move(send)),
      options_(options),
      reassembler_(options.maxPayloadBytes) {}

std::shared_ptr<MultiFrameTransfer> MultiFrameTransfer::start(Micro target, std::vector<std::uint8_t> requestPayload,
                                                              VcuSend send, MultiFrameOptions options) {
  std::shared_ptr<MultiFrameTransfer> transfer(
      new MultiFrameTransfer(target, std::move(requestPayload), std::move(send), options));
  transfer->begin();
  return transfer;
}

void MultiFrameTransfer::begin() {
  std::vector<std::array<std::uint8_t, 8>> frames = segmentRequestPayload(target_, requestPayload_);
  if (frames.empty()) {
    settle({MultiFrameResult::Kind::kNotSent, {}, false, TransferStage::kFirstReply, "an empty request", {}});
    return;
  }
  outstanding_.assign(frames.begin() + 1, frames.end());
  if (!transmit(frames.front())) return;
  if (outstanding_.empty()) {
    armTimer(options_.firstReplyTimeoutMs, TransferStage::kFirstReply);
  } else {
    armTimer(options_.requestFlowControlTimeoutMs, TransferStage::kRequestFlowControl);
  }
}

bool MultiFrameTransfer::handleFrame(std::span<const std::uint8_t> data, ArrivalStamp arrival) {
  if (settled_ || data.size() < 2 || data[0] != kTesterAddress) return false;
  const int budget = maxFramesFor(options_.maxPayloadBytes) + kFlowControlFrameAllowance;
  if (framesHandled_ >= budget) {
    settle({MultiFrameResult::Kind::kAbandoned, {}, false, TransferStage::kFirstReply,
            "more than " + std::to_string(budget) + " frames in one exchange", {}});
    return true;
  }
  framesHandled_ += 1;
  if (const std::optional<VcuFlowControl> flowControl = parseFlowControlFrame(data)) {
    return handleFlowControlFromMicro(*flowControl);
  }
  ExtendedIsoTpResult result = reassembler_.push(data);
  switch (result.status) {
    case ExtendedIsoTpResult::Status::kFlowControlRequired:
      if (!transmit(buildFlowControlFrame(target_))) return true;
      flowControlLatency_ = arrivalLatencyMs(arrival, now());
      armTimer(options_.transferTimeoutMs, TransferStage::kReplyTransfer);
      return true;
    case ExtendedIsoTpResult::Status::kIncomplete:
      return true;
    case ExtendedIsoTpResult::Status::kComplete:
      settle({MultiFrameResult::Kind::kPayload, std::move(result.payload), sawFlowControlFromMicro_,
              TransferStage::kFirstReply, {}, {}});
      return true;
    case ExtendedIsoTpResult::Status::kAbandoned:
      settle({MultiFrameResult::Kind::kAbandoned, {}, false, TransferStage::kFirstReply, result.reason, {}});
      return true;
    case ExtendedIsoTpResult::Status::kIgnored:
      return false;
  }
  return false;
}

bool MultiFrameTransfer::handleFlowControlFromMicro(const VcuFlowControl& flowControl) {
  if (outstanding_.empty()) {
    if (!sawStrayFlowControl_) {
      logInfo(kTag, "flow control from %s with no request frames outstanding, ignoring", microName(target_));
    }
    sawStrayFlowControl_ = true;
    return true;
  }
  sawFlowControlFromMicro_ = true;
  switch (flowControl.status) {
    case VcuFlowControl::Status::kClearToSend:
      async::currentLoop().clearTimeout(timer_);
      timer_ = {};
      consecutiveFramesUntilNextFlowControl_ = flowControl.blockSize;
      separationTimeMs_ = flowControl.separationTimeMs;
      async::currentLoop().clearTimeout(pacer_);
      pacer_ = {};
      sendNextRequestFrame();
      return true;
    case VcuFlowControl::Status::kWait:
      armTimer(options_.requestFlowControlTimeoutMs, TransferStage::kRequestFlowControl);
      return true;
    case VcuFlowControl::Status::kOverflow:
      settle({MultiFrameResult::Kind::kAbandoned, {}, false, TransferStage::kFirstReply,
              std::string(microName(target_)) +
                  " answered our request with flow-control OVERFLOW — it cannot hold the request",
              {}});
      return true;
    case VcuFlowControl::Status::kUnrecognised:
      settle({MultiFrameResult::Kind::kAbandoned, {}, false, TransferStage::kFirstReply,
              "flow control with undefined status " + hex(flowControl.flowStatus), {}});
      return true;
  }
  return true;
}

void MultiFrameTransfer::onRequestFlowControlTimeout() {
  logWarning(kTag,
             "%s sent no flow control for our %zu-byte request within %lld ms — sending the remaining %zu consecutive "
             "frame(s) unprompted. See onRequestFlowControlTimeout in src/vcu/multiframe-transfer.ts for why that is "
             "the chosen guess.",
             microName(target_), requestPayload_.size(), static_cast<long long>(options_.requestFlowControlTimeoutMs),
             outstanding_.size());
  consecutiveFramesUntilNextFlowControl_ = 0;
  separationTimeMs_ = 0;
  sendNextRequestFrame();
}

void MultiFrameTransfer::sendNextRequestFrame() {
  if (settled_ || outstanding_.empty()) return;
  const std::array<std::uint8_t, 8> frame = outstanding_.front();
  outstanding_.erase(outstanding_.begin());
  if (!transmit(frame)) return;
  if (outstanding_.empty()) {
    armTimer(options_.firstReplyTimeoutMs, TransferStage::kFirstReply);
    return;
  }
  if (consecutiveFramesUntilNextFlowControl_ > 0) {
    consecutiveFramesUntilNextFlowControl_ -= 1;
    if (consecutiveFramesUntilNextFlowControl_ == 0) {
      armTimer(options_.requestFlowControlTimeoutMs, TransferStage::kRequestFlowControl);
      return;
    }
  }
  std::weak_ptr<MultiFrameTransfer> self = weak_from_this();
  pacer_ = async::currentLoop().setTimeout(separationTimeMs_, [self] {
    if (const auto transfer = self.lock()) transfer->sendNextRequestFrame();
  });
}

bool MultiFrameTransfer::transmit(const std::array<std::uint8_t, 8>& frame) {
  if (const std::optional<std::string> problem = send_(kKwpRequestCanId, frame)) {
    logError(kTag, "sending %s to %s failed: %s", toHex(frame).c_str(), microName(target_), problem->c_str());
    settle({MultiFrameResult::Kind::kNotSent, {}, false, TransferStage::kFirstReply, *problem, {}});
    return false;
  }
  return true;
}

void MultiFrameTransfer::armTimer(std::int64_t ms, TransferStage stage) {
  async::currentLoop().clearTimeout(timer_);
  // The transfer stays alive while its timer can fire: the client may have let go of it.
  std::shared_ptr<MultiFrameTransfer> self = shared_from_this();
  timer_ = async::currentLoop().setTimeout(ms, [self, stage] {
    self->timer_ = {};
    if (stage == TransferStage::kRequestFlowControl) {
      self->onRequestFlowControlTimeout();
      return;
    }
    self->settle({MultiFrameResult::Kind::kTimeout, {}, false, stage, {}, {}});
  });
}

void MultiFrameTransfer::settle(MultiFrameResult result) {
  if (settled_) return;
  settled_ = true;
  async::currentLoop().clearTimeout(timer_);
  async::currentLoop().clearTimeout(pacer_);
  timer_ = {};
  pacer_ = {};
  reassembler_.reset();
  result.flowControlLatency = flowControlLatency_;
  finished_.resolve(std::move(result));
}

void MultiFrameTransfer::cancel(std::string reason) {
  settle({MultiFrameResult::Kind::kCancelled, {}, false, TransferStage::kFirstReply, std::move(reason), {}});
}

// --- The client ---------------------------------------------------------------------------------

struct VcuKwpClient::State {
  explicit State(VcuSend sendFrame) : send(std::move(sendFrame)) {}

  struct Pending {
    std::shared_ptr<MultiFrameTransfer> transfer;  // a multi-frame exchange, or null for a single frame
    std::function<void(Exchange)> settle;
    async::TimerId timer;
  };

  VcuSend send;
  std::optional<Pending> pending;
  std::optional<double> lastExchangeAt[2];
  std::optional<std::uint32_t> pendingResponseCanId;
  bool stopped = false;
};

VcuKwpClient::VcuKwpClient(VcuSend send) : state_(std::make_shared<State>(std::move(send))) {}

std::optional<std::string> VcuKwpClient::busyReason(State& state) {
  if (state.stopped) return std::string("client stopped");
  if (state.pending) {
    const std::string reason = "a request was already in flight";
    logWarning(kTag, "%s — refusing to interleave a second one", reason.c_str());
    return reason;
  }
  return std::nullopt;
}

bool VcuKwpClient::handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) {
  State& state = *state_;
  if (!state.pendingResponseCanId || id != *state.pendingResponseCanId) return false;
  if (state.pending && state.pending->transfer) return state.pending->transfer->handleFrame(data, arrival);
  VcuFrame frame = parseResponseFrame(data);
  if (frame.kind == VcuFrame::Kind::kIgnored) return false;
  if (!state.pending) return true;
  State::Pending waiting = std::move(*state.pending);
  state.pending.reset();
  async::currentLoop().clearTimeout(waiting.timer);
  waiting.settle({Exchange::Kind::kReply, std::move(frame), {}});
  return true;
}

async::Task<VcuKwpClient::Exchange> VcuKwpClient::exchange(std::shared_ptr<State> state, Micro micro,
                                                          VcuRequest request) {
  if (state->stopped) co_return Exchange{Exchange::Kind::kNotSent, {}, "client stopped"};
  if (const std::optional<std::string> busy = busyReason(*state)) co_return Exchange{Exchange::Kind::kNotSent, {}, *busy};
  const std::array<std::uint8_t, 8> frame = buildRequestFrame(micro, request);
  async::Deferred<Exchange> result;
  // Settled, the reply is handed over a pace later, so the next request never follows too closely.
  const auto settle = [state, micro, result](Exchange outcome) {
    if (outcome.kind == Exchange::Kind::kReply) state->lastExchangeAt[microSlot(micro)] = now();
    state->pendingResponseCanId.reset();
    async::currentLoop().setTimeout(kPaceMs, [result, outcome]() mutable { result.resolve(std::move(outcome)); });
  };
  const async::TimerId timer = async::currentLoop().setTimeout(kResponseTimeoutMs, [state, settle] {
    state->pending.reset();
    settle({Exchange::Kind::kTimeout, {}, {}});
  });
  state->pending = State::Pending{nullptr, settle, timer};
  state->pendingResponseCanId = kKwpResponseCanId;
  if (const std::optional<std::string> problem = state->send(kKwpRequestCanId, frame)) {
    async::currentLoop().clearTimeout(timer);
    state->pending.reset();
    logError(kTag, "send failed: %s", problem->c_str());
    settle({Exchange::Kind::kNotSent, {}, *problem});
  }
  co_return co_await result;
}

async::Task<MultiFrameResult> VcuKwpClient::runTransfer(std::shared_ptr<State> state, Micro micro,
                                                        std::vector<std::uint8_t> payload, MultiFrameOptions options) {
  if (const std::optional<std::string> busy = busyReason(*state)) {
    co_return MultiFrameResult{MultiFrameResult::Kind::kNotSent, {}, false, TransferStage::kFirstReply, *busy, {}};
  }
  const VcuSend send = state->send;
  const std::shared_ptr<MultiFrameTransfer> transfer = MultiFrameTransfer::start(micro, std::move(payload), send, options);
  state->pending = State::Pending{transfer, nullptr, {}};
  state->pendingResponseCanId = kKwpResponseCanId;
  MultiFrameResult result = co_await transfer->finished();
  if (state->pending && state->pending->transfer == transfer) state->pending.reset();
  state->pendingResponseCanId.reset();
  if (result.kind == MultiFrameResult::Kind::kPayload) state->lastExchangeAt[microSlot(micro)] = now();
  co_await async::sleep(kPaceMs);
  co_return result;
}

async::Task<VcuKwpClient::SessionAttempt> VcuKwpClient::openSessionAttempt(std::shared_ptr<State> state, Micro micro) {
  const Exchange result = co_await exchange(state, micro, {RequestKind::kStartSession});
  const bool opened = result.kind == Exchange::Kind::kReply && result.frame.kind == VcuFrame::Kind::kPayload &&
                      isSessionOpened(result.frame.payload);
  state->lastExchangeAt[microSlot(micro)] = opened ? std::optional<double>(now()) : std::nullopt;
  if (opened) co_return SessionAttempt{true, std::nullopt};
  co_return SessionAttempt{false, result.kind == Exchange::Kind::kNotSent ? std::optional<std::string>(result.reason)
                                                                          : std::nullopt};
}

async::Task<VcuKwpClient::SessionAttempt> VcuKwpClient::ensureSession(std::shared_ptr<State> state, Micro micro) {
  const std::optional<double> last = state->lastExchangeAt[microSlot(micro)];
  if (last && now() - *last < kSessionIdleLimitMs) co_return SessionAttempt{true, std::nullopt};
  co_return co_await openSessionAttempt(state, micro);
}

async::Task<bool> VcuKwpClient::openSession(Micro micro) {
  co_return (co_await openSessionAttempt(state_, micro)).opened;
}

async::Task<bool> VcuKwpClient::ping(Micro micro) {
  const std::shared_ptr<State> state = state_;
  if (!(co_await ensureSession(state, micro)).opened) co_return false;
  const Exchange result = co_await exchange(state, micro, {RequestKind::kTesterPresent});
  co_return result.kind == Exchange::Kind::kReply && result.frame.kind == VcuFrame::Kind::kPayload;
}

namespace {

VcuReadResult sessionFailure(const std::optional<std::string>& notSent, Micro micro, const char* when) {
  VcuReadResult result;
  if (notSent) {
    result.status = ReadStatus::kNotSent;
    result.reason = *notSent;
  } else {
    result.status = ReadStatus::kNoSession;
    result.reason = std::string(microName(micro)) + " " + when;
  }
  return result;
}

VcuReadResult describeReadResult(const MultiFrameResult& transferred, int identifier) {
  VcuReadResult result;
  switch (transferred.kind) {
    case MultiFrameResult::Kind::kPayload: {
      const VcuParameterReply reply = decodeParameterReply(transferred.payload, identifier);
      switch (reply.kind) {
        case VcuParameterReply::Kind::kRecord:
          result.status = ReadStatus::kRead;
          result.record = reply.record;
          break;
        case VcuParameterReply::Kind::kRefused:
          result.status = ReadStatus::kRefused;
          result.negativeResponseCode = reply.negativeResponseCode;
          result.description = reply.description;
          break;
        case VcuParameterReply::Kind::kIdentifierMismatch:
          result.status = ReadStatus::kUnrecognised;
          result.reason = "reply echoed identifier " + hex(reply.received) + ", not " + hex(reply.expected);
          break;
        case VcuParameterReply::Kind::kUnrecognised:
          result.status = ReadStatus::kUnrecognised;
          result.reason = reply.reason;
          break;
      }
      return result;
    }
    case MultiFrameResult::Kind::kTimeout:
      switch (transferred.stage) {
        case TransferStage::kFirstReply:
          result.status = ReadStatus::kNoResponse;
          return result;
        case TransferStage::kReplyTransfer:
          result.status = ReadStatus::kStalled;
          result.reason = "a First Frame arrived and the rest of the reply never did";
          return result;
        case TransferStage::kRequestFlowControl:
          result.status = ReadStatus::kStalled;
          result.reason = "no flow control for a request of ours — impossible for a read, which is always one frame";
          return result;
      }
      return result;
    case MultiFrameResult::Kind::kAbandoned:
      result.status = ReadStatus::kAbandoned;
      result.reason = transferred.reason;
      return result;
    case MultiFrameResult::Kind::kCancelled:
    case MultiFrameResult::Kind::kNotSent:
      result.status = ReadStatus::kNotSent;
      result.reason = transferred.reason;
      return result;
  }
  return result;
}

}  // namespace

async::Task<VcuReadResult> VcuKwpClient::performRead(std::shared_ptr<State> state, Micro micro, int bank, int index) {
  const int identifier = identifierFor(bank, index).value_or(0);
  if (const std::optional<std::string> busy = busyReason(*state)) {
    VcuReadResult result;
    result.status = ReadStatus::kNotSent;
    result.reason = *busy;
    co_return result;
  }
  const SessionAttempt session = co_await ensureSession(state, micro);
  if (!session.opened) co_return sessionFailure(session.notSent, micro, "did not answer 10 81");
  const std::vector<std::uint8_t> payload = buildRequestPayload({RequestKind::kReadParameter, bank, index});
  MultiFrameOptions options;
  options.maxPayloadBytes = kReadMaxPayloadBytes;
  options.firstReplyTimeoutMs = kResponseTimeoutMs;
  MultiFrameResult result = co_await runTransfer(state, micro, payload, options);
  // Silence at the first reply may be a session that lapsed: open it again, and ask once more.
  if (result.kind == MultiFrameResult::Kind::kTimeout && result.stage == TransferStage::kFirstReply) {
    const SessionAttempt reopened = co_await openSessionAttempt(state, micro);
    if (!reopened.opened) co_return sessionFailure(reopened.notSent, micro, "stopped answering 10 81 mid-read");
    result = co_await runTransfer(state, micro, payload, options);
  }
  VcuReadResult described = describeReadResult(result, identifier);
  described.flowControlLatency = result.flowControlLatency;
  co_return described;
}

async::Task<VcuReadOutcome> VcuKwpClient::readParameter(Micro micro, int index) {
  co_return co_await probe(micro, kCalibrationBank, index);
}

async::Task<VcuReadOutcome> VcuKwpClient::probe(Micro target, int bank, int index) {
  VcuReadOutcome outcome;
  static_cast<VcuReadResult&>(outcome) = co_await performRead(state_, target, bank, index);
  outcome.micro = target;
  outcome.bank = bank;
  outcome.index = index;
  outcome.identifier = identifierFor(bank, index).value_or(0);
  co_return outcome;
}

const char* multiFrameStatusName(VcuMultiFrameOutcome::Status status) {
  switch (status) {
    case VcuMultiFrameOutcome::Status::kReply:
      return "reply";
    case VcuMultiFrameOutcome::Status::kNoResponse:
      return "no-response";
    case VcuMultiFrameOutcome::Status::kAbandoned:
      return "abandoned";
    case VcuMultiFrameOutcome::Status::kNoSession:
      return "no-session";
    case VcuMultiFrameOutcome::Status::kCancelled:
      return "cancelled";
    case VcuMultiFrameOutcome::Status::kNotSent:
      return "not-sent";
  }
  return "not-sent";
}

async::Task<VcuMultiFrameOutcome> VcuKwpClient::multiFrameRead(Micro target, VcuMultiFrameRequest request,
                                                              MultiFrameOptions options) {
  const std::shared_ptr<State> state = state_;
  VcuMultiFrameOutcome outcome;
  if (const std::optional<std::string> busy = busyReason(*state)) {
    outcome.reason = *busy;
    co_return outcome;
  }
  const SessionAttempt session = co_await ensureSession(state, target);
  if (!session.opened) {
    if (session.notSent) {
      outcome.reason = *session.notSent;
    } else {
      outcome.status = VcuMultiFrameOutcome::Status::kNoSession;
      outcome.reason = std::string(microName(target)) + " did not answer 10 81";
    }
    co_return outcome;
  }
  const int expected = expectedResponseService(request);
  MultiFrameResult result = co_await runTransfer(state, target, encodeMultiFrameRequestPayload(request), options);
  switch (result.kind) {
    case MultiFrameResult::Kind::kPayload:
      outcome.status = VcuMultiFrameOutcome::Status::kReply;
      outcome.reply = decodeMultiFrameReply(result.payload, expected);
      outcome.payload = std::move(result.payload);
      outcome.sawFlowControlFromMicro = result.sawFlowControlFromMicro;
      outcome.flowControlLatency = result.flowControlLatency;
      break;
    case MultiFrameResult::Kind::kTimeout:
      outcome.status = VcuMultiFrameOutcome::Status::kNoResponse;
      outcome.stage = result.stage;
      outcome.flowControlLatency = result.flowControlLatency;
      break;
    case MultiFrameResult::Kind::kAbandoned:
      outcome.status = VcuMultiFrameOutcome::Status::kAbandoned;
      outcome.reason = result.reason;
      outcome.flowControlLatency = result.flowControlLatency;
      break;
    case MultiFrameResult::Kind::kCancelled:
      outcome.status = VcuMultiFrameOutcome::Status::kCancelled;
      outcome.reason = result.reason;
      break;
    case MultiFrameResult::Kind::kNotSent:
      outcome.reason = result.reason;
      break;
  }
  co_return outcome;
}

bool VcuKwpClient::cancelMultiFrameRead(std::string reason) {
  if (!state_->pending || !state_->pending->transfer) return false;
  state_->pending->transfer->cancel(std::move(reason));
  return true;
}

void VcuKwpClient::stop() {
  State& state = *state_;
  state.stopped = true;
  state.pendingResponseCanId.reset();
  if (!state.pending) return;
  State::Pending waiting = std::move(*state.pending);
  state.pending.reset();
  if (waiting.transfer) {
    waiting.transfer->cancel("client stopped");
    return;
  }
  async::currentLoop().clearTimeout(waiting.timer);
  waiting.settle({Exchange::Kind::kNotSent, {}, "client stopped"});
}

}  // namespace emcool::vcu
