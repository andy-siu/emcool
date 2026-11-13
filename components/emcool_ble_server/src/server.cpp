#include "emcool/ble_server/server.hpp"

#include <cctype>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::ble_server {

namespace {

using text::TextBuffer;

constexpr std::string_view kBadPath = "path must start with /";
constexpr std::string_view kBodyTooLarge = "the request body is larger than the bike accepts";

constexpr const char* kTag = "ble-server";
// MTU − 5, as the reference sends: it also fits BlueZ's multiple-notification format, so the phone sees
// the same frame sizes from either bike. Never above ATT's 512-byte value ceiling.
constexpr std::uint16_t kNotificationOverheadBytes = 5;
constexpr std::size_t kMaxNotificationBytes = 512;
constexpr std::uint8_t kCompleteFrameFlags = 3;
constexpr std::uint8_t kClearEnvelopeTag = 0;

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t at = 0; at < left.size(); at++) {
    if (std::tolower(static_cast<unsigned char>(left[at])) != std::tolower(static_cast<unsigned char>(right[at]))) {
      return false;
    }
  }
  return true;
}

void copyTruncated(std::string_view from, char* to, std::size_t capacity) {
  const std::size_t length = std::min(from.size(), capacity - 1);
  std::memcpy(to, from.data(), length);
  to[length] = '\0';
}

}  // namespace

BleServer::BleServer(std::string_view secret, signals::SignalStore& store, std::mutex& storeMutex,
                     RequestHandler& requests)
    : session_(secret), store_(store), storeMutex_(storeMutex), requests_(requests) {}

void BleServer::connected() { mtu_ = kDefaultAttMtu; }

void BleServer::disconnected() {
  endSession("phone disconnected");
  mtu_ = kDefaultAttMtu;
}

void BleServer::notificationsEnabled(bool enabled) {
  if (!enabled) {
    endSession("phone turned notifications off");
  }
}

void BleServer::mtuChanged(std::uint16_t mtu) {
  if (mtu >= kDefaultAttMtu) {
    mtu_ = mtu;
  }
}

void BleServer::receiveFrame(std::span<const std::uint8_t> frame, std::int64_t nowMonotonicMs) {
  if (frame.size() < ble::kFrameHeaderBytes || frame.size() > ble::kMaxWriteFrameBytes) {
    noteProblem("invalid frame", nowMonotonicMs);
    return;
  }
  // Before authentication only a whole HELLO in one frame counts, so a stranger can never hold
  // the assembler.
  if (frame[0] == kCompleteFrameFlags && frame.size() > ble::kFrameHeaderBytes &&
      frame[ble::kFrameHeaderBytes] == kClearEnvelopeTag) {
    receiveHello(frame.subspan(ble::kFrameHeaderBytes), nowMonotonicMs);
    return;
  }
  if (!session_.authenticated()) {
    noteProblem("only a complete HELLO may precede authentication", nowMonotonicMs);
    return;
  }
  const ble::FrameAssembler::Result assembled = assembler_.push(frame);
  if (assembled.problem != nullptr) {
    noteProblem(assembled.problem, nowMonotonicMs);
  }
  if (!assembled.envelope) return;
  const ble::BleSession::Received result = session_.receive(*assembled.envelope);
  if (result.problem != nullptr) {
    noteProblem(result.problem, nowMonotonicMs);
  }
  if (result.message) {
    dispatch(*result.message, nowMonotonicMs);
  }
}

void BleServer::receiveHello(std::span<const std::uint8_t> envelope, std::int64_t nowMonotonicMs) {
  // Replies (REFUSED, WELCOME) are written straight into the next free slot.
  Slot* slot = freeSlot();
  const ble::BleSession::Received result =
      session_.receiveHello(envelope, slot != nullptr ? std::span<std::uint8_t>(slot->bytes) : std::span<std::uint8_t>());
  if (result.problem != nullptr) {
    noteProblem(result.problem, nowMonotonicMs);
  }
  if (result.startedSession) {
    // Whatever was queued belonged to the session this one replaces.
    counters_.sessionsStarted++;
    streaming_ = false;
    stream_.cancelSnapshot();
    requestCount_ = 0;
    if (responseStage_ != ResponseStage::kNone) requests_.abandon();
    responseStage_ = ResponseStage::kNone;
    assembler_.reset();
    if (slot != nullptr) {
      slotHead_ = static_cast<std::size_t>(slot - slots_.data());
    }
    slotCount_ = 0;
    splitter_.reset();
    attempt_.reset();
    logInfo(kTag, "phone authenticated, session started");
  }
  if (result.replyLength == 0 || slot == nullptr) return;
  // With no lockout, refusals are what a flood of wrong HELLOs would multiply; the reference allows a
  // burst of four per 2 s.
  if (!result.startedSession && !allowRefusal(nowMonotonicMs)) return;
  slot->length = result.replyLength;
  commitSlot(*slot);
  if (!result.startedSession) {
    counters_.refusals++;
  }
}

bool BleServer::allowRefusal(std::int64_t nowMonotonicMs) {
  if (!refusalWindowStartMs_ || nowMonotonicMs - *refusalWindowStartMs_ >= kRejectionWindowMs) {
    refusalWindowStartMs_ = nowMonotonicMs;
    refusalsInWindow_ = 0;
  }
  if (refusalsInWindow_ >= kRefusalsPerWindow) return false;
  refusalsInWindow_++;
  return true;
}

// One log line per window, with the count and the latest reason, so a peer's flood can't bury the
// console; tick() flushes a quiet tail.
void BleServer::noteProblem(const char* reason, std::int64_t nowMonotonicMs) {
  counters_.problems++;
  pendingProblems_++;
  lastProblem_ = reason;
  if (!lastProblemLogMs_ || nowMonotonicMs - *lastProblemLogMs_ >= kRejectionWindowMs) {
    flushProblems(nowMonotonicMs);
  }
}

void BleServer::flushProblems(std::int64_t nowMonotonicMs) {
  if (pendingProblems_ == 0) return;
  if (pendingProblems_ == 1) {
    logInfo(kTag, "rejected: %s", lastProblem_);
  } else {
    logInfo(kTag, "rejected %u frames/messages, the latest: %s", static_cast<unsigned>(pendingProblems_),
            lastProblem_);
  }
  pendingProblems_ = 0;
  lastProblemLogMs_ = nowMonotonicMs;
}

void BleServer::tick(std::int64_t nowMonotonicMs, std::int64_t nowWallClockMs) {
  if (pendingProblems_ > 0 && lastProblemLogMs_ && nowMonotonicMs - *lastProblemLogMs_ >= kRejectionWindowMs) {
    flushProblems(nowMonotonicMs);
  }
  if (!session_.authenticated()) return;
  advanceResponse(nowMonotonicMs);
  advanceStream(nowMonotonicMs, nowWallClockMs);
}

bool BleServer::nextFrame(std::span<std::uint8_t> out, std::size_t& length) {
  while (slotCount_ > 0) {
    Slot& head = slots_[slotHead_];
    if (!splitter_) {
      splitter_.emplace(std::span<const std::uint8_t>(head.bytes.data(), head.length), head.messageId,
                        std::min<std::size_t>(mtu_ - kNotificationOverheadBytes, kMaxNotificationBytes));
    }
    attempt_ = *splitter_;
    if (attempt_->next(out, length)) {
      attemptLength_ = length;
      return true;
    }
    // Nothing left of this envelope (it can't happen, but a stuck slot would stop the link).
    slotHead_ = (slotHead_ + 1) % kOutboundSlots;
    slotCount_--;
    splitter_.reset();
    attempt_.reset();
  }
  return false;
}

void BleServer::frameSent() {
  if (!attempt_) return;
  splitter_ = attempt_;
  attempt_.reset();
  counters_.framesSent++;
  counters_.bytesSent += attemptLength_;
  if (splitter_->done()) {
    slotHead_ = (slotHead_ + 1) % kOutboundSlots;
    slotCount_--;
    splitter_.reset();
  }
}

std::size_t BleServer::queuedBytes() const {
  std::size_t bytes = 0;
  for (std::size_t index = 0; index < slotCount_; index++) {
    bytes += slots_[(slotHead_ + index) % kOutboundSlots].length;
  }
  return bytes;
}

void BleServer::endSession(const char* reason) {
  const bool hadSession = session_.authenticated() || streaming_;
  if (responseStage_ != ResponseStage::kNone) requests_.abandon();
  slotCount_ = 0;
  splitter_.reset();
  attempt_.reset();
  assembler_.reset();
  requestCount_ = 0;
  responseStage_ = ResponseStage::kNone;
  streaming_ = false;
  stream_.cancelSnapshot();
  if (!hadSession) return;
  session_.end();
  logInfo(kTag, "session ended (%s)", reason);
}

void BleServer::dispatch(const ble::Message& message, std::int64_t nowMonotonicMs) {
  switch (static_cast<ble::MessageKind>(message.kind)) {
    case ble::MessageKind::kRequest:
      queueRequest(message.header, message.body);
      return;
    case ble::MessageKind::kSubscribe:
      if (streaming_) return;
      streaming_ = true;
      {
        // Patches carry changes from here on; the snapshot carries everything before.
        const std::lock_guard<std::mutex> lock(storeMutex_);
        store_.clearAllChanged();
      }
      stream_.beginSnapshot();
      counters_.snapshots++;
      lastSnapshotMs_ = nowMonotonicMs;
      lastPatchMs_ = nowMonotonicMs;
      return;
    case ble::MessageKind::kUnsubscribe:
      streaming_ = false;
      stream_.cancelSnapshot();
      return;
    default:
      logInfo(kTag, "ignoring message kind %u", static_cast<unsigned>(message.kind));
      return;
  }
}

void BleServer::queueRequest(std::string_view header, std::span<const std::uint8_t> body) {
  ble::JsonObjectReader reader;
  char scratch[512];
  const ble::JsonMember* id = nullptr;
  if (!reader.read(header, scratch) || (id = reader.find("id")) == nullptr || id->type != ble::JsonType::kNumber) {
    counters_.problems++;
    logInfo(kTag, "a REQUEST without a readable header or id");
    return;
  }
  if (requestCount_ == kMaxQueuedRequests) {
    counters_.problems++;
    logWarning(kTag, "%zu requests already waiting; dropping one", kMaxQueuedRequests);
    return;
  }
  PendingRequest& request = requestQueue_[requestCount_++];
  request.id = static_cast<std::int64_t>(id->number);
  const ble::JsonMember* method = reader.find("method");
  const ble::JsonMember* path = reader.find("path");
  copyTruncated(method != nullptr && method->type == ble::JsonType::kString ? method->text : "GET",
                request.method.data(), request.method.size());
  copyTruncated(path != nullptr && path->type == ble::JsonType::kString ? path->text : "", request.path.data(),
                request.path.size());
  request.emcoolHeader[0] = '\0';
  request.bodyTooLarge = body.size() > kMaxRequestBodyBytes;
  request.body.assign(request.bodyTooLarge ? std::string_view() : std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
  if (const ble::JsonMember* headers = reader.find("headers"); headers != nullptr && headers->type == ble::JsonType::kObject) {
    ble::JsonObjectReader headerReader;
    char headerScratch[512];
    if (headerReader.read(headers->text, headerScratch)) {
      for (const ble::JsonMember& member : headerReader.members()) {
        if (member.type == ble::JsonType::kString && equalsIgnoringCase(member.key, "x-emcool")) {
          copyTruncated(member.text, request.emcoolHeader.data(), request.emcoolHeader.size());
        }
      }
    }
  }
}

BleServer::Slot* BleServer::freeSlot() {
  if (slotCount_ == kOutboundSlots) return nullptr;
  return &slots_[(slotHead_ + slotCount_) % kOutboundSlots];
}

void BleServer::commitSlot(Slot& slot, bool response) {
  slot.messageId = ++nextMessageId_;
  slot.response = response;
  slotCount_++;
}

// A long response body (a ride-log download is megabytes) and the live stream share the slots: while
// both have something to send, neither may hold every one. The stream keeps its messages coming
// (the dashboard calls a link dead after 12 s without one), and a snapshot never stalls the body.
bool BleServer::mayTakeSlot(bool response) const {
  if (slotCount_ == kOutboundSlots) return false;
  const bool contended = response ? streaming_ : responseStage_ == ResponseStage::kBody;
  if (!contended) return true;
  std::size_t held = 0;
  for (std::size_t index = 0; index < slotCount_; index++) {
    if (slots_[(slotHead_ + index) % kOutboundSlots].response == response) held++;
  }
  return held + 1 < kOutboundSlots;
}

bool BleServer::sealIntoSlot(ble::MessageKind kind, std::string_view header, std::span<const std::uint8_t> body) {
  Slot* slot = freeSlot();
  if (slot == nullptr) return false;
  const std::size_t length = session_.seal(kind, header, body, slot->bytes);
  if (length == 0) {
    counters_.problems++;
    logWarning(kTag, "could not seal a message of kind %u", static_cast<unsigned>(kind));
    return false;
  }
  slot->length = length;
  commitSlot(*slot, kind == ble::MessageKind::kResponseHead || kind == ble::MessageKind::kResponseBody ||
                        kind == ble::MessageKind::kResponseEnd);
  return true;
}

// One request at a time: its head, its body in pieces, its end — each as a slot frees up.
void BleServer::advanceResponse(std::int64_t nowMonotonicMs) {
  while (true) {
    if (responseStage_ == ResponseStage::kNone) {
      if (requestCount_ == 0) return;
      const PendingRequest request = std::move(requestQueue_[0]);
      std::move(requestQueue_.begin() + 1, requestQueue_.begin() + requestCount_, requestQueue_.begin());
      requestCount_--;
      counters_.requests++;
      logInfo(kTag, "%s %s", request.method.data(), request.path.data());
      responseId_ = request.id;
      responseOffset_ = 0;
      bodyShortBy_ = 0;
      ownBody_ = request.path[0] != '/' ? kBadPath : request.bodyTooLarge ? kBodyTooLarge : std::string_view();
      if (!ownBody_.empty()) {
        response_ = {request.path[0] != '/' ? 400 : 413, "text/plain; charset=utf-8", ownBody_.size()};
        responseStage_ = ResponseStage::kHead;
      } else {
        const std::optional<HttpResponse> response = requests_.begin(
            {request.method.data(), request.path.data(), request.emcoolHeader.data(),
             request.body},
            nowMonotonicMs);
        if (response) {
          response_ = *response;
          responseStage_ = ResponseStage::kHead;
        } else {
          responseStage_ = ResponseStage::kWaiting;
          waitingSinceMs_ = nowMonotonicMs;
        }
      }
    }
    if (responseStage_ == ResponseStage::kWaiting) {
      std::optional<HttpResponse> response = requests_.poll(nowMonotonicMs);
      // A handler gets three minutes.
      if (!response && nowMonotonicMs - waitingSinceMs_ > kRequestTimeoutMs) {
        logWarning(kTag, "request %lld had no answer after %lld s", static_cast<long long>(responseId_),
                   static_cast<long long>(kRequestTimeoutMs / 1000));
        response = requests_.timedOut();
      }
      if (!response) return;
      response_ = *response;
      responseStage_ = ResponseStage::kHead;
    }
    if (!mayTakeSlot(true)) return;

    TextBuffer header(header_.data(), header_.size());
    header.append("{\"id\":");
    header.appendInteger(responseId_);
    std::span<const std::uint8_t> body;
    ble::MessageKind kind = ble::MessageKind::kResponseEnd;
    ResponseStage next = ResponseStage::kNone;
    if (responseStage_ == ResponseStage::kHead) {
      header.append(",\"status\":");
      header.appendInteger(response_.status);
      header.append(",\"headers\":{\"content-type\":");
      header.appendJsonString(response_.contentType);
      header.append(",\"content-length\":\"");
      header.appendUnsigned(response_.bodyLength);
      header.append("\",\"cache-control\":\"no-store\"");
      if (response_.fileName != nullptr) {
        header.append(",\"content-disposition\":");
        header.appendJsonString(std::string("attachment; filename=\"") + response_.fileName + "\"");
      }
      header.append("}");
      kind = ble::MessageKind::kResponseHead;
      next = response_.bodyLength > 0 ? ResponseStage::kBody : ResponseStage::kEnd;
    } else if (responseStage_ == ResponseStage::kBody) {
      const std::size_t wanted = std::min(kResponseChunkBytes, response_.bodyLength - responseOffset_);
      const std::span<const char> piece =
          !ownBody_.empty() ? std::span<const char>(ownBody_.substr(responseOffset_, wanted))
                            : requests_.body(responseOffset_, wanted);
      if (piece.empty()) {
        // The handler came up short of the length it promised: end it, rather than loop, and say so
        // in the END, so the phone never hands over a truncated file as whole.
        bodyShortBy_ = response_.bodyLength - responseOffset_;
        logWarning(kTag, "the body of request %lld ended %zu bytes early", static_cast<long long>(responseId_),
                   bodyShortBy_);
        responseOffset_ = response_.bodyLength;
      }
      body = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(piece.data()), piece.size());
      responseOffset_ += piece.size();
      kind = ble::MessageKind::kResponseBody;
      next = responseOffset_ < response_.bodyLength ? ResponseStage::kBody : ResponseStage::kEnd;
      // An empty piece carries nothing; go straight on to the END.
      if (piece.empty()) {
        responseStage_ = ResponseStage::kEnd;
        continue;
      }
    } else if (bodyShortBy_ > 0) {
      header.append(",\"error\":\"the bike's answer ended ");
      header.appendUnsigned(bodyShortBy_);
      header.append(" bytes short of its length\"");
    }
    header.append("}");
    if (header.overflowed() || !sealIntoSlot(kind, std::string_view(header_.data(), header.length()), body)) {
      logWarning(kTag, "dropping the response to request %lld", static_cast<long long>(responseId_));
      requests_.abandon();
      responseStage_ = ResponseStage::kNone;
      continue;
    }
    responseStage_ = next;
  }
}

void BleServer::advanceStream(std::int64_t nowMonotonicMs, std::int64_t nowWallClockMs) {
  if (!streaming_) return;
  while (stream_.snapshotInProgress() && mayTakeSlot(false)) {
    std::size_t length = 0;
    {
      const std::lock_guard<std::mutex> lock(storeMutex_);
      length = stream_.nextSnapshotChunk(store_, nowWallClockMs, header_);
    }
    if (length == 0) break;
    sealIntoSlot(ble::MessageKind::kStream, std::string_view(header_.data(), length), {});
  }

  // At most one patch window's worth per 100 ms; whatever doesn't fit a free slot stays marked.
  if (nowMonotonicMs - lastPatchMs_ >= kPatchWindowMs && mayTakeSlot(false)) {
    lastPatchMs_ = nowMonotonicMs;
    while (mayTakeSlot(false)) {
      std::size_t length = 0;
      {
        const std::lock_guard<std::mutex> lock(storeMutex_);
        length = stream_.nextPatch(store_, nowWallClockMs, header_);
      }
      if (length == 0) break;
      sealIntoSlot(ble::MessageKind::kStream, std::string_view(header_.data(), length), {});
    }
  }

  if (nowMonotonicMs - lastSnapshotMs_ >= kHeartbeatMs && !stream_.snapshotInProgress()) {
    lastSnapshotMs_ = nowMonotonicMs;
    stream_.beginSnapshot();
    counters_.snapshots++;
  }
}

}  // namespace emcool::ble_server
