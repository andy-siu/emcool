#include "emcool/obd/elock.hpp"

#include <algorithm>

#include "emcool/log.hpp"

namespace emcool::obd {

namespace {

constexpr const char* kTag = "elock";
constexpr std::uint8_t kServiceReadByLocalId = 0x21;
constexpr std::uint8_t kServiceStartSession = 0x10;
constexpr std::uint8_t kServiceStopSession = 0x20;
constexpr std::uint8_t kKeysPairedLocalId = 0x99;
constexpr std::uint8_t kStandardSession = 0x81;
constexpr std::uint8_t kNegativeResponseSid = 0x7f;
constexpr std::uint8_t kPositiveResponseOffset = 0x40;

// A positive reply's data after the service id (possibly empty), or nullopt for anything else;
// a refusal is logged with its NRC.
std::optional<std::span<const std::uint8_t>> positiveResponsePayload(
    std::optional<std::span<const std::uint8_t>> response, std::uint8_t service) {
  if (!response || response->size() < 2) return std::nullopt;
  const std::span<const std::uint8_t> payload =
      response->subspan(1, std::min<std::size_t>((*response)[0], response->size() - 1));
  if (payload.empty()) return std::nullopt;
  if (payload[0] == kNegativeResponseSid) {
    const unsigned negativeResponseCode = payload.size() > 2 ? payload[2] : 0;
    logInfo(kTag, "negative response to service 0x%x (NRC 0x%x)", service, negativeResponseCode);
    return std::nullopt;
  }
  if (payload[0] != service + kPositiveResponseOffset) return std::nullopt;
  return payload.subspan(1);
}

std::optional<int> keysPairedFrom(std::optional<std::span<const std::uint8_t>> response) {
  const auto payload = positiveResponsePayload(response, kServiceReadByLocalId);
  if (!payload || payload->size() < 2 || (*payload)[0] != kKeysPairedLocalId) return std::nullopt;
  return (*payload)[1];
}

}  // namespace

void ElockReader::start(Now now) {
  keysPaired_.reset();
  send(Step::kReadKeys, now);
}

void ElockReader::send(Step step, Now now) {
  step_ = step;
  Payload frame = {};  // zero padding
  if (step == Step::kStartSession) {
    frame[0] = 2;
    frame[1] = kServiceStartSession;
    frame[2] = kStandardSession;
  } else if (step == Step::kStopSession) {
    frame[0] = 1;
    frame[1] = kServiceStopSession;
  } else {
    frame[0] = 2;
    frame[1] = kServiceReadByLocalId;
    frame[2] = kKeysPairedLocalId;
  }
  if (const std::optional<std::string> problem = bus_.transmit(kRequestId, frame)) {
    logError(kTag, "send failed: %s", problem->c_str());
    waiting_ = false;
    answered(std::nullopt, now);
    return;
  }
  waiting_ = true;
  deadline_ = order_.at(now.monotonicMs + kResponseTimeoutMs);
}

void ElockReader::handleResponse(std::span<const std::uint8_t> data, Now now) {
  if (!waiting_) return;
  waiting_ = false;
  answered(data, now);
}

void ElockReader::tick(Now now) {
  if (!waiting_ || now.monotonicMs < deadline_.atMs) return;
  waiting_ = false;
  answered(std::nullopt, now);
}

std::optional<Deadline> ElockReader::nextDeadline() const {
  if (!waiting_) return std::nullopt;
  return deadline_;
}

void ElockReader::answered(std::optional<std::span<const std::uint8_t>> response, Now now) {
  switch (step_) {
    case Step::kReadKeys:
      if (const std::optional<int> keys = keysPairedFrom(response)) {
        finish(keys);
      } else {
        // The default session wouldn't say; try a standard one.
        send(Step::kStartSession, now);
      }
      return;
    case Step::kStartSession:
      if (positiveResponsePayload(response, kServiceStartSession)) {
        send(Step::kReadKeysInSession, now);
      } else {
        finish(std::nullopt);
      }
      return;
    case Step::kReadKeysInSession:
      keysPaired_ = keysPairedFrom(response);
      send(Step::kStopSession, now);
      return;
    case Step::kStopSession:
      finish(keysPaired_);
      return;
    case Step::kIdle:
    case Step::kDone:
      return;
  }
}

void ElockReader::finish(std::optional<int> keysPaired) {
  step_ = Step::kDone;
  if (!keysPaired) {
    logInfo(kTag, "no usable response to 21 99 — skipping keys_paired");
    return;
  }
  recorder_.record("keys_paired", *keysPaired);
  logInfo(kTag, "keys_paired = %d", *keysPaired);
}

}  // namespace emcool::obd
