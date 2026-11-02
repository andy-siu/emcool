#include "emcool/obd/trouble_code_reader.hpp"

#include <cmath>
#include <cstdio>

#include "emcool/log.hpp"

namespace emcool::obd {

namespace {

constexpr const char* kTag = "obd-dtc";
constexpr std::uint32_t kResponseLowId = 0x7e0;
constexpr std::uint32_t kResponseHighId = 0x7ef;
// Flow control goes to the responder's physical request id, 8 below its response id.
constexpr std::uint32_t kPhysicalRequestLowId = 0x7e0;
constexpr std::uint32_t kPhysicalRequestHighId = 0x7e7;
constexpr std::uint32_t kFlowControlIdOffset = 8;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

}  // namespace

std::string describeReadOutcome(const DtcReadOutcome& result) {
  char modeText[16];
  std::snprintf(modeText, sizeof modeText, "mode 0x%02x", result.mode);
  const std::string mode = modeText;
  switch (result.outcome) {
    case DtcReadOutcome::Kind::kSilent:
      return mode + ": NO RESPONSE — which is not the same claim as “no codes”";
    case DtcReadOutcome::Kind::kNotSent:
      return mode + ": never asked — " + result.reason;
    case DtcReadOutcome::Kind::kTruncated:
      return mode + ": transfer started and stalled — " + result.reason;
    case DtcReadOutcome::Kind::kAnswered:
      break;
  }
  const ObdDtcResponse& response = result.response;
  if (response.kind == ObdDtcResponse::Kind::kNegative) {
    return mode + ": refused, NRC " + describeNegativeResponseCode(response.negativeResponseCode);
  }
  if (response.kind == ObdDtcResponse::Kind::kUnrecognised) {
    return mode + ": unrecognised reply — " + response.reason;
  }
  const std::string mismatch =
      response.truncated ? " (count byte said " + std::to_string(response.declaredCount) + ")" : "";
  return mode + ": " + std::to_string(response.codes.size()) + " " + listName(response.list) + " code(s)" + mismatch;
}

std::optional<DtcReadOutcome> TroubleCodeReader::start(int mode, Now now) {
  if (!listForMode(mode)) {
    logError(kTag, "mode %s is not a read-only trouble-code service", hex(mode).c_str());
    return DtcReadOutcome{DtcReadOutcome::Kind::kNotSent, mode, {}, "not a read-only trouble-code service"};
  }
  if (busy()) {
    const std::string reason = "a mode " + hex(mode_) + " read was still running";
    logWarning(kTag, "mode %s skipped — %s", hex(mode).c_str(), reason.c_str());
    return DtcReadOutcome{DtcReadOutcome::Kind::kNotSent, mode, {}, reason};
  }
  mode_ = mode;
  attempt_ = 0;
  attempt(now);
  return std::nullopt;
}

void TroubleCodeReader::attempt(Now now) {
  phase_ = Phase::kAttempt;
  attemptStartedMs_ = now.monotonicMs;
  deadline_ = order_.at(now.monotonicMs + kFirstReplyTimeoutMs);
  sawFirstFrame_ = false;
  failure_.reset();
  reassembler_.reset();
  Payload frame = {};
  frame[0] = 0x01;
  frame[1] = static_cast<std::uint8_t>(mode_);
  if (const std::optional<std::string> problem = bus_.transmit(kFunctionalRequestId, frame)) {
    logError(kTag, "send of mode %s failed: %s", hex(mode_).c_str(), problem->c_str());
    attemptEnded({DtcReadOutcome::Kind::kNotSent, mode_, {}, *problem}, now);
  }
}

bool TroubleCodeReader::handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, Now now) {
  if (phase_ != Phase::kAttempt || id < kResponseLowId || id > kResponseHighId || !addressesService(data)) {
    return false;
  }
  IsoTpResult result = reassembler_.push(data);
  switch (result.status) {
    case IsoTpStatus::kFlowControlRequired:
      sendFlowControl(id);
      sawFirstFrame_ = true;
      deadline_ = order_.at(now.monotonicMs + kTransferTimeoutMs);
      return true;
    case IsoTpStatus::kIncomplete:
      return true;
    case IsoTpStatus::kComplete: {
      DtcReadOutcome answered{DtcReadOutcome::Kind::kAnswered, mode_, decodeObdDtcResponse(result.payload, mode_), {}};
      reassembler_.reset();
      attemptEnded(std::move(answered), now);
      return true;
    }
    case IsoTpStatus::kAbandoned:
      // Kept for the stall report; the timer still decides when this attempt is over.
      failure_ = std::move(result.reason);
      return true;
    case IsoTpStatus::kIgnored:
      return false;
  }
  return false;
}

void TroubleCodeReader::tick(Now now) {
  if (phase_ == Phase::kIdle || now.monotonicMs < deadline_.atMs) return;
  if (phase_ == Phase::kAttempt) {
    expire(now);
  } else if (phase_ == Phase::kRetryGap) {
    attempt(now);
  }
}

std::optional<Deadline> TroubleCodeReader::nextDeadline() const {
  if (phase_ == Phase::kIdle) return std::nullopt;
  return deadline_;
}

std::optional<DtcReadOutcome> TroubleCodeReader::takeOutcome() {
  std::optional<DtcReadOutcome> outcome = std::move(outcome_);
  outcome_.reset();
  return outcome;
}

void TroubleCodeReader::expire(Now now) {
  reassembler_.reset();
  if (!sawFirstFrame_) {
    attemptEnded({DtcReadOutcome::Kind::kSilent, mode_, {}, {}}, now);
    return;
  }
  attemptEnded(
      {DtcReadOutcome::Kind::kTruncated, mode_, {}, failure_.value_or("no consecutive frames after the first frame")},
      now);
}

void TroubleCodeReader::attemptEnded(DtcReadOutcome result, Now now) {
  if (result.outcome != DtcReadOutcome::Kind::kTruncated) {
    phase_ = Phase::kIdle;
    outcome_ = std::move(result);
    return;
  }
  logInfo(kTag, "mode %s stalled after %lld ms (%s) — attempt %d of %d", hex(mode_).c_str(),
          static_cast<long long>(now.monotonicMs - attemptStartedMs_), result.reason.c_str(), attempt_ + 1,
          kRetryAttempts + 1);
  if (attempt_ < kRetryAttempts) {
    attempt_ += 1;
    phase_ = Phase::kRetryGap;
    deadline_ = order_.at(now.monotonicMs + kRetryGapMs);
    return;
  }
  phase_ = Phase::kIdle;
  outcome_ = std::move(result);
}

// Whether a frame belongs to this read: a consecutive frame (it carries no service id), or a
// single or first frame whose service answers ours, positively or with a refusal.
bool TroubleCodeReader::addressesService(std::span<const std::uint8_t> data) const {
  if (data.size() < 2) return false;
  const int pci = data[0] >> 4;
  if (pci == 0x2) return true;
  // A first frame's service id is behind its two-byte length.
  const int service = pci == 0x1 ? (data.size() > 2 ? data[2] : -1) : data[1];
  if (service == mode_ + 0x40) return true;
  if (service == 0x7f && data.size() >= 3) {
    const int refused = pci == 0x1 ? (data.size() > 3 ? data[3] : -1) : data[2];
    return refused == mode_;
  }
  return false;
}

void TroubleCodeReader::sendFlowControl(std::uint32_t responseId) {
  const std::uint32_t flowControlId = responseId - kFlowControlIdOffset;
  if (flowControlId < kPhysicalRequestLowId || flowControlId > kPhysicalRequestHighId) {
    logWarning(kTag, "refusing flow control to 0x%x — outside the physical request range",
               static_cast<unsigned>(flowControlId));
    return;
  }
  // Clear to send, no block limit, no gap.
  const Payload frame = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
  if (const std::optional<std::string> problem = bus_.transmit(flowControlId, frame)) {
    logError(kTag, "flow control to 0x%x failed: %s", static_cast<unsigned>(flowControlId), problem->c_str());
  }
}

}  // namespace emcool::obd
