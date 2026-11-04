#include "emcool/vcu/write_session.hpp"

#include <cmath>
#include <cstdio>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr std::int64_t kResponseTimeoutMs = 300;
constexpr std::int64_t kPaceMs = 10;
constexpr std::uint8_t kServiceEcuReset = 0x11;
constexpr std::uint8_t kResetKeyOffOn = 0x02;
constexpr std::uint8_t kPositiveResetResponse = 0x51;
constexpr std::uint8_t kNegativeResponseService = 0x7f;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

const char* frameKindName(VcuFrame::Kind kind) {
  switch (kind) {
    case VcuFrame::Kind::kPayload:
      return "payload";
    case VcuFrame::Kind::kMultiFrame:
      return "multi-frame";
    case VcuFrame::Kind::kIgnored:
      return "ignored";
  }
  return "ignored";
}

std::string describeResetNrc(int code) {
  const char* name = "unknown";
  switch (code) {
    case 0x11:
      name = "serviceNotSupported";
      break;
    case 0x12:
      name = "subFunctionNotSupported";
      break;
    case 0x22:
      name = "conditionsNotCorrect";
      break;
    case 0x31:
      name = "requestOutOfRange";
      break;
    case 0x33:
      name = "securityAccessDenied";
      break;
    default:
      break;
  }
  char text[16];
  std::snprintf(text, sizeof text, "0x%02x", code);
  return std::string("NRC ") + text + " (" + name + ")";
}

std::string upperHex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "%X", value);
  return text;
}

}  // namespace

const char* writeStageName(WriteStage stage) {
  switch (stage) {
    case WriteStage::kSession:
      return "session";
    case WriteStage::kReadBefore:
      return "read-before";
    case WriteStage::kSecuritySeed:
      return "security-seed";
    case WriteStage::kSecurityKey:
      return "security-key";
    case WriteStage::kWrite:
      return "write";
    case WriteStage::kReadBack:
      return "read-back";
    case WriteStage::kRoutine:
      return "routine";
  }
  return "session";
}

std::string describeExchangeFailure(bool timeout, const std::string& reason) {
  return timeout ? "no reply within the 300 ms window" : "never reached the bus — " + reason;
}

double SecurityCooldown::remainingMs(Micro micro, double nowMs) const {
  const auto found = marks_.find(micro);
  if (found == marks_.end()) return 0;
  const double remaining = kSecurityCooldownMs - (nowMs - found->second);
  return remaining > 0 ? remaining : 0;
}

void SecurityCooldown::mark(Micro micro, double nowMs) { marks_[micro] = nowMs; }

WriteSession::ExchangeResult WriteSession::ExchangeResult::reply(VcuFrame frame) {
  return {Kind::kReply, std::move(frame), {}};
}
WriteSession::ExchangeResult WriteSession::ExchangeResult::timeout() { return {Kind::kTimeout, {}, {}}; }
WriteSession::ExchangeResult WriteSession::ExchangeResult::notSent(std::string reason) {
  return {Kind::kNotSent, {}, std::move(reason)};
}

double WriteSession::nowMs() const { return static_cast<double>(async::currentLoop().now()); }

bool WriteSession::handleFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
  if (!pending_) return false;
  if (pendingIsObd_) {
    if (id < 0x7e0 || id > 0x7ef) return false;
    if (!isClearDtcsReply(data)) return false;
    auto settle = pending_->settle;
    async::currentLoop().clearTimeout(pending_->timer);
    pending_.reset();
    VcuFrame frame;
    frame.kind = VcuFrame::Kind::kPayload;
    frame.payload.assign(data.begin(), data.end());
    settle(ExchangeResult::reply(std::move(frame)));
    return true;
  }
  if (!pendingResponseCanId_ || id != *pendingResponseCanId_) return false;
  VcuFrame frame = parseResponseFrame(data);
  if (frame.kind == VcuFrame::Kind::kIgnored) return false;
  auto settle = pending_->settle;
  async::currentLoop().clearTimeout(pending_->timer);
  pending_.reset();
  settle(ExchangeResult::reply(std::move(frame)));
  return true;
}

void WriteSession::abort(std::string reason) {
  stopped_ = true;
  stoppedReason_ = std::move(reason);
  pendingResponseCanId_.reset();
  if (pending_) {
    auto settle = pending_->settle;
    async::currentLoop().clearTimeout(pending_->timer);
    pending_.reset();
    VcuFrame empty;
    empty.kind = VcuFrame::Kind::kPayload;
    settle(ExchangeResult::reply(std::move(empty)));
  }
}

async::Task<WriteSession::ExchangeResult> WriteSession::exchangeRaw(std::uint32_t requestCanId,
                                                                    std::optional<std::uint32_t> responseCanId,
                                                                    std::array<std::uint8_t, 8> frame, bool isObd) {
  if (stopped_) co_return ExchangeResult::notSent(stoppedReason_.empty() ? "session stopped" : stoppedReason_);
  if (pending_) {
    logWarning(kTag, "a request was already in flight — refusing to interleave a second one");
    co_return ExchangeResult::notSent("a request was already in flight");
  }
  async::Deferred<ExchangeResult> done;
  auto self = shared_from_this();
  auto settle = [self, done](ExchangeResult result) mutable {
    self->pendingResponseCanId_.reset();
    self->pendingIsObd_ = false;
    if (self->unlockedMicro_) self->cooldown_.mark(*self->unlockedMicro_, self->nowMs());
    async::currentLoop().setTimeout(kPaceMs, [done, result]() mutable { done.resolve(result); });
  };
  async::TimerId timer = async::currentLoop().setTimeout(kResponseTimeoutMs, [self, settle]() mutable {
    self->pending_.reset();
    settle(ExchangeResult::timeout());
  });
  pending_ = Pending{[settle](ExchangeResult result) mutable { settle(result); }, timer};
  pendingResponseCanId_ = responseCanId;
  pendingIsObd_ = isObd;
  if (const std::optional<std::string> problem = send_(requestCanId, frame)) {
    async::currentLoop().clearTimeout(timer);
    pending_.reset();
    settle(ExchangeResult::notSent(*problem));
  }
  co_return co_await done;
}

async::Task<WriteSession::ExchangeResult> WriteSession::exchangeRead(Micro micro, VcuRequest request) {
  co_return co_await exchangeRaw(kKwpRequestCanId, kKwpResponseCanId, buildRequestFrame(micro, request), false);
}

async::Task<WriteSession::ExchangeResult> WriteSession::exchangeWrite(Micro micro, VcuWriteRequest request) {
  const WriteFrameResult built = buildWriteFrame(names_, micro, request);
  if (!built.ok) co_return ExchangeResult::notSent(built.reason);
  co_return co_await exchangeRaw(kKwpRequestCanId, kKwpResponseCanId, built.frame, false);
}

async::Task<bool> WriteSession::openSession(Micro micro) {
  VcuRequest request;
  request.kind = RequestKind::kStartSession;
  const ExchangeResult result = co_await exchangeRead(micro, request);
  co_return result.kind == ExchangeResult::Kind::kReply && result.frame.kind == VcuFrame::Kind::kPayload &&
      isSessionOpened(result.frame.payload);
}

// A raw record read by identifier (no interpretation).
namespace {
struct IdentifierRead {
  bool ok = false;
  std::vector<std::uint8_t> record;
  std::string reason;
};
}  // namespace

async::Task<WriteSession::ParameterRead> WriteSession::readParameterValue(Micro micro, int index) {
  VcuRequest request;
  request.kind = RequestKind::kReadParameter;
  request.bank = 1;
  request.index = index;
  const ExchangeResult result = co_await exchangeRead(micro, request);
  ParameterRead read;
  if (result.kind != ExchangeResult::Kind::kReply) {
    read.reason = describeExchangeFailure(result.kind == ExchangeResult::Kind::kTimeout, result.reason);
    co_return read;
  }
  if (result.frame.kind != VcuFrame::Kind::kPayload) {
    read.reason = std::string("the reply was a ") + frameKindName(result.frame.kind) + " frame";
    co_return read;
  }
  const int identifier = static_cast<int>(identifierFor(1, index).value_or(0));
  const VcuParameterReply reply = decodeParameterReply(result.frame.payload, identifier);
  switch (reply.kind) {
    case VcuParameterReply::Kind::kRefused:
      read.reason = "refused with NRC " + reply.description;
      co_return read;
    case VcuParameterReply::Kind::kIdentifierMismatch:
      read.reason = "the reply answered " + hex(reply.received) + ", not " + hex(reply.expected);
      co_return read;
    case VcuParameterReply::Kind::kUnrecognised:
      read.reason = reply.reason;
      co_return read;
    case VcuParameterReply::Kind::kRecord:
      break;
  }
  const VcuParameter* parameter = names_.atIndex(index);
  std::optional<RecordEncoding> encoding;
  if (parameter != nullptr) encoding = RecordEncoding{parameter->type, parameter->isSigned, nullptr};
  const VcuParameterValue interpreted = interpretRecord(reply.record, encoding);
  if (!interpreted.value) {
    read.reason = "the reply " + toHex(reply.record) +
                  " does not match what params.ecf says this parameter's width and sign are";
    co_return read;
  }
  read.ok = true;
  read.value = *interpreted.value;
  read.rawHex = interpreted.rawHex;
  co_return read;
}

async::Task<WriteSession::UnlockResult> WriteSession::unlock(Micro micro) {
  cooldown_.mark(micro, nowMs());
  UnlockResult result;
  VcuWriteRequest seedRequest;
  seedRequest.kind = VcuWriteRequest::Kind::kSecuritySeed;
  const ExchangeResult seedExchange = co_await exchangeWrite(micro, seedRequest);
  if (seedExchange.kind != ExchangeResult::Kind::kReply) {
    result.stage = WriteStage::kSecuritySeed;
    result.reason = describeExchangeFailure(seedExchange.kind == ExchangeResult::Kind::kTimeout, seedExchange.reason);
    co_return result;
  }
  if (seedExchange.frame.kind != VcuFrame::Kind::kPayload) {
    result.stage = WriteStage::kSecuritySeed;
    result.reason = std::string("the reply was a ") + frameKindName(seedExchange.frame.kind) + " frame";
    co_return result;
  }
  const SecurityAccessReply seedReply = decodeSecurityAccessReply(seedExchange.frame.payload);
  if (seedReply.kind == SecurityAccessReply::Kind::kRefused) {
    result.kind = UnlockResult::Kind::kRefused;
    result.stage = WriteStage::kSecuritySeed;
    result.negativeResponseCode = seedReply.negativeResponseCode;
    result.description = seedReply.description;
    co_return result;
  }
  if (seedReply.kind != SecurityAccessReply::Kind::kSeed) {
    result.stage = WriteStage::kSecuritySeed;
    result.reason = seedReply.kind == SecurityAccessReply::Kind::kUnlocked
                        ? "the micro reports it is already unlocked"
                        : seedReply.reason;
    co_return result;
  }
  VcuWriteRequest keyRequest;
  keyRequest.kind = VcuWriteRequest::Kind::kSecurityKey;
  keyRequest.key = securityKeyForSeed(seedReply.seed);
  const ExchangeResult keyExchange = co_await exchangeWrite(micro, keyRequest);
  if (keyExchange.kind != ExchangeResult::Kind::kReply) {
    result.stage = WriteStage::kSecurityKey;
    result.reason = describeExchangeFailure(keyExchange.kind == ExchangeResult::Kind::kTimeout, keyExchange.reason);
    co_return result;
  }
  if (keyExchange.frame.kind != VcuFrame::Kind::kPayload) {
    result.stage = WriteStage::kSecurityKey;
    result.reason = std::string("the reply was a ") + frameKindName(keyExchange.frame.kind) + " frame";
    co_return result;
  }
  const SecurityAccessReply keyReply = decodeSecurityAccessReply(keyExchange.frame.payload);
  if (keyReply.kind == SecurityAccessReply::Kind::kRefused) {
    result.kind = UnlockResult::Kind::kRefused;
    result.stage = WriteStage::kSecurityKey;
    result.negativeResponseCode = keyReply.negativeResponseCode;
    result.description =
        keyReply.negativeResponseCode == 0x35
            ? keyReply.description + " — that is one of about three attempts, and the lockout clears only on a VCU "
                                     "power cycle"
            : keyReply.description;
    co_return result;
  }
  if (keyReply.kind != SecurityAccessReply::Kind::kUnlocked) {
    result.stage = WriteStage::kSecurityKey;
    result.reason = keyReply.kind == SecurityAccessReply::Kind::kSeed
                        ? "the micro answered the key with another seed instead of accepting it — it is not unlocked"
                        : keyReply.reason;
    co_return result;
  }
  unlockedMicro_ = micro;
  result.kind = UnlockResult::Kind::kUnlocked;
  co_return result;
}

async::Task<ServiceWriteOutcome> WriteSession::writeAndReadBack(Micro micro, ParameterWritePlan plan,
                                                               const TableTypeReport* report, bool present) {
  ServiceWriteOutcome outcome;
  outcome.plan = plan;
  VcuWriteRequest request;
  request.kind = VcuWriteRequest::Kind::kWriteParameter;
  request.plan = plan;
  request.tableType = report;
  request.tableTypePresent = present;
  const ExchangeResult written = co_await exchangeWrite(micro, request);
  if (written.kind != ExchangeResult::Kind::kReply) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kWrite;
    outcome.reason = describeExchangeFailure(written.kind == ExchangeResult::Kind::kTimeout, written.reason);
    co_return outcome;
  }
  if (written.frame.kind != VcuFrame::Kind::kPayload) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kWrite;
    outcome.reason = std::string("the reply was a ") + frameKindName(written.frame.kind) + " frame";
    co_return outcome;
  }
  const ParameterWriteReply reply = decodeParameterWriteReply(written.frame.payload, plan.identifier);
  if (reply.kind == ParameterWriteReply::Kind::kRefused) {
    outcome.status = ServiceWriteOutcome::Status::kRefused;
    outcome.stage = WriteStage::kWrite;
    outcome.negativeResponseCode = reply.negativeResponseCode;
    outcome.description = reply.negativeResponseCode == 0x33
                             ? reply.description + " — the unlock went stale before the write landed; try again"
                             : reply.description;
    co_return outcome;
  }
  if (reply.kind != ParameterWriteReply::Kind::kAccepted) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kWrite;
    outcome.reason = reply.kind == ParameterWriteReply::Kind::kIdentifierMismatch
                         ? "the micro acknowledged identifier " + hex(reply.received) + ", not the " +
                               hex(reply.expected) + " we wrote — nothing here can say what was changed"
                         : reply.reason;
    co_return outcome;
  }
  const ParameterRead after = co_await readParameterValue(micro, plan.index);
  if (!after.ok) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kReadBack;
    outcome.reason = "the micro accepted the write and then would not say what it holds: " + after.reason;
    co_return outcome;
  }
  outcome.readBack = after.value;
  outcome.rawHex = after.rawHex;
  outcome.status = after.value == plan.value ? ServiceWriteOutcome::Status::kWritten
                                             : ServiceWriteOutcome::Status::kReadBackMismatch;
  co_return outcome;
}

async::Task<ServiceWriteOutcome> WriteSession::parameterWrite(ParameterWritePlan plan, const TableTypeReport* report,
                                                             bool present) {
  ServiceWriteOutcome outcome;
  outcome.plan = plan;
  const Micro micro = plan.micro;
  const double cooldown = cooldown_.remainingMs(micro, nowMs());
  if (cooldown > 0) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kSession;
    char seconds[16];
    std::snprintf(seconds, sizeof seconds, "%g", std::round((kSecurityCooldownMs - cooldown) / 100) / 10);
    outcome.reason = std::string(microName(micro)) + " was authenticated " + seconds + " s ago — wait " +
                     std::to_string(static_cast<long long>(std::ceil(cooldown / 1000))) +
                     " s. Asking a micro that is still unlocked for a seed returns invalidKey and spends one of about "
                     "three attempts before it locks out until the bike is power-cycled.";
    co_return outcome;
  }
  if (!(co_await openSession(micro))) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kSession;
    outcome.reason = std::string(microName(micro)) + " did not answer 10 81";
    co_return outcome;
  }
  const ParameterRead before = co_await readParameterValue(micro, plan.index);
  if (!before.ok) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kReadBefore;
    outcome.reason = before.reason;
    co_return outcome;
  }
  if (before.value != plan.previousValue) {
    outcome.status = ServiceWriteOutcome::Status::kStalePrecondition;
    outcome.actual = before.value;
    co_return outcome;
  }
  if (before.value == plan.value) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = WriteStage::kReadBefore;
    outcome.reason = plan.name + " already reads " + text::jsNumber(plan.value) + " — nothing to write";
    co_return outcome;
  }
  const UnlockResult unlocked = co_await unlock(micro);
  if (unlocked.kind == UnlockResult::Kind::kRefused) {
    outcome.status = ServiceWriteOutcome::Status::kRefused;
    outcome.stage = unlocked.stage;
    outcome.negativeResponseCode = unlocked.negativeResponseCode;
    outcome.description = unlocked.description;
    co_return outcome;
  }
  if (unlocked.kind == UnlockResult::Kind::kFailed) {
    outcome.status = ServiceWriteOutcome::Status::kFailed;
    outcome.stage = unlocked.stage;
    outcome.reason = unlocked.reason;
    co_return outcome;
  }
  co_return co_await writeAndReadBack(micro, plan, report, present);
}

namespace {
std::string batchStopReason(const ServiceWriteOutcome& outcome) {
  switch (outcome.status) {
    case ServiceWriteOutcome::Status::kWritten:
    case ServiceWriteOutcome::Status::kReadBackMismatch:
    case ServiceWriteOutcome::Status::kStalePrecondition:
      return {};
    case ServiceWriteOutcome::Status::kRefused:
      return outcome.negativeResponseCode == 0x33
                 ? "the " + std::string(microName(outcome.plan.micro)) + " unlock went stale at " + outcome.plan.name +
                       " (0x33) — not sending a second 27"
                 : std::string();
    case ServiceWriteOutcome::Status::kFailed:
      return outcome.stage == WriteStage::kWrite || outcome.stage == WriteStage::kReadBack
                 ? "the bus stopped answering at " + outcome.plan.name + " (" + writeStageName(outcome.stage) + ")"
                 : std::string();
  }
  return {};
}
}  // namespace

async::Task<BatchWriteOutcome> WriteSession::parameterWrites(std::vector<ParameterWritePlan> plans,
                                                            const TableTypeReport* report, bool present) {
  BatchWriteOutcome batch;
  if (plans.empty()) {
    batch.stage = WriteStage::kSession;
    batch.reason = "a batch write was asked to write nothing";
    co_return batch;
  }
  const Micro micro = plans.front().micro;
  for (const ParameterWritePlan& plan : plans) {
    if (plan.micro != micro) {
      batch.stage = WriteStage::kSession;
      batch.reason = "a single-session batch must be one micro";
      co_return batch;
    }
  }
  const double cooldown = cooldown_.remainingMs(micro, nowMs());
  if (cooldown > 0) {
    batch.stage = WriteStage::kSession;
    char seconds[16];
    std::snprintf(seconds, sizeof seconds, "%g", std::round((kSecurityCooldownMs - cooldown) / 100) / 10);
    batch.reason = std::string(microName(micro)) + " was authenticated " + seconds + " s ago — wait " +
                   std::to_string(static_cast<long long>(std::ceil(cooldown / 1000))) +
                   " s. Asking a micro that is still unlocked for a seed returns invalidKey and spends one of about "
                   "three attempts before it locks out until the bike is power-cycled.";
    co_return batch;
  }
  if (!(co_await openSession(micro))) {
    batch.stage = WriteStage::kSession;
    batch.reason = std::string(microName(micro)) + " did not answer 10 81";
    co_return batch;
  }
  std::vector<std::optional<ServiceWriteOutcome>> results(plans.size());
  std::vector<std::size_t> toWrite;
  for (std::size_t position = 0; position < plans.size(); position++) {
    const ParameterWritePlan& plan = plans[position];
    const ParameterRead before = co_await readParameterValue(micro, plan.index);
    if (!before.ok) {
      ServiceWriteOutcome outcome;
      outcome.plan = plan;
      outcome.status = ServiceWriteOutcome::Status::kFailed;
      outcome.stage = WriteStage::kReadBefore;
      outcome.reason = before.reason;
      results[position] = outcome;
      continue;
    }
    if (before.value != plan.previousValue) {
      ServiceWriteOutcome outcome;
      outcome.plan = plan;
      outcome.status = ServiceWriteOutcome::Status::kStalePrecondition;
      outcome.actual = before.value;
      results[position] = outcome;
      continue;
    }
    if (before.value == plan.value) {
      ServiceWriteOutcome outcome;
      outcome.plan = plan;
      outcome.status = ServiceWriteOutcome::Status::kFailed;
      outcome.stage = WriteStage::kReadBefore;
      outcome.reason = plan.name + " already reads " + text::jsNumber(plan.value) + " — nothing to write";
      results[position] = outcome;
      continue;
    }
    toWrite.push_back(position);
  }
  if (!toWrite.empty()) {
    const UnlockResult unlocked = co_await unlock(micro);
    if (unlocked.kind != UnlockResult::Kind::kUnlocked) {
      for (const std::size_t position : toWrite) {
        ServiceWriteOutcome outcome;
        outcome.plan = plans[position];
        if (unlocked.kind == UnlockResult::Kind::kRefused) {
          outcome.status = ServiceWriteOutcome::Status::kRefused;
          outcome.stage = unlocked.stage;
          outcome.negativeResponseCode = unlocked.negativeResponseCode;
          outcome.description = unlocked.description;
        } else {
          outcome.status = ServiceWriteOutcome::Status::kFailed;
          outcome.stage = unlocked.stage;
          outcome.reason = unlocked.reason;
        }
        results[position] = outcome;
      }
    } else {
      std::string stopReason;
      for (const std::size_t position : toWrite) {
        if (!stopReason.empty()) {
          ServiceWriteOutcome outcome;
          outcome.plan = plans[position];
          outcome.status = ServiceWriteOutcome::Status::kFailed;
          outcome.stage = WriteStage::kWrite;
          outcome.reason = "not attempted — " + stopReason;
          results[position] = outcome;
          continue;
        }
        ServiceWriteOutcome outcome = co_await writeAndReadBack(micro, plans[position], report, present);
        stopReason = batchStopReason(outcome);
        results[position] = outcome;
      }
    }
  }
  batch.status = BatchWriteOutcome::Status::kRan;
  for (std::optional<ServiceWriteOutcome>& outcome : results) batch.results.push_back(std::move(*outcome));
  co_return batch;
}

async::Task<StampRead> WriteSession::readStampInSession(double nowMs) {
  StampRead read;
  ServiceStampRaw raw;
  int* fields[] = {&raw.dateLow, &raw.dateHigh, &raw.odometerLow, &raw.odometerHigh};
  std::size_t at = 0;
  for (const ServiceStampIdentifier& entry : serviceStampIdentifiers()) {
    const int bank = entry.identifier >> 12;
    const int index = entry.identifier & 0x0fff;
    VcuRequest request;
    request.kind = RequestKind::kReadParameter;
    request.bank = bank;
    request.index = index;
    const ExchangeResult result = co_await exchangeRead(Micro::kA8, request);
    bool okRecord = false;
    std::vector<std::uint8_t> record;
    std::string reason;
    if (result.kind != ExchangeResult::Kind::kReply) {
      reason = describeExchangeFailure(result.kind == ExchangeResult::Kind::kTimeout, result.reason);
    } else if (result.frame.kind != VcuFrame::Kind::kPayload) {
      reason = std::string("the reply was a ") + frameKindName(result.frame.kind) + " frame";
    } else {
      const VcuParameterReply reply = decodeParameterReply(result.frame.payload, entry.identifier);
      switch (reply.kind) {
        case VcuParameterReply::Kind::kRecord:
          okRecord = true;
          record = reply.record;
          break;
        case VcuParameterReply::Kind::kRefused:
          reason = "refused with NRC " + reply.description;
          break;
        case VcuParameterReply::Kind::kIdentifierMismatch:
          reason = "the reply answered " + hex(reply.received) + ", not " + hex(reply.expected);
          break;
        case VcuParameterReply::Kind::kUnrecognised:
          reason = reply.reason;
          break;
      }
    }
    if (!okRecord || record.size() != 2) {
      read.reason = std::string(entry.field) + ": " + (okRecord ? "wrong width" : reason);
      co_return read;
    }
    *fields[at++] = (record[0] << 8) | record[1];
  }
  read.ok = true;
  read.stamp = interpretServiceStamp(raw, nowMs);
  co_return read;
}

async::Task<StampRead> WriteSession::readServiceStamp(double nowMs) {
  StampRead read;
  if (!(co_await openSession(Micro::kA8))) {
    read.reason = std::string(kServiceStampMicro) + " did not answer 10 81";
    co_return read;
  }
  ServiceStampRaw raw;
  int* fields[] = {&raw.dateLow, &raw.dateHigh, &raw.odometerLow, &raw.odometerHigh};
  std::size_t at = 0;
  for (const ServiceStampIdentifier& entry : serviceStampIdentifiers()) {
    const int bank = entry.identifier >> 12;
    const int index = entry.identifier & 0x0fff;
    VcuRequest request;
    request.kind = RequestKind::kReadParameter;
    request.bank = bank;
    request.index = index;
    const ExchangeResult result = co_await exchangeRead(Micro::kA8, request);
    bool okRecord = false;
    std::vector<std::uint8_t> record;
    std::string reason;
    if (result.kind != ExchangeResult::Kind::kReply) {
      reason = describeExchangeFailure(result.kind == ExchangeResult::Kind::kTimeout, result.reason);
    } else if (result.frame.kind != VcuFrame::Kind::kPayload) {
      reason = std::string("the reply was a ") + frameKindName(result.frame.kind) + " frame";
    } else {
      const VcuParameterReply reply = decodeParameterReply(result.frame.payload, entry.identifier);
      switch (reply.kind) {
        case VcuParameterReply::Kind::kRecord:
          okRecord = true;
          record = reply.record;
          break;
        case VcuParameterReply::Kind::kRefused:
          reason = "refused with NRC " + reply.description;
          break;
        case VcuParameterReply::Kind::kIdentifierMismatch:
          reason = "the reply answered " + hex(reply.received) + ", not " + hex(reply.expected);
          break;
        case VcuParameterReply::Kind::kUnrecognised:
          reason = reply.reason;
          break;
      }
    }
    if (!okRecord) {
      read.reason = std::string(entry.field) + " (identifier 0x" + upperHex(entry.identifier) + ") — " + reason +
                    ". A8 answered all four of these on 2026-09-08, so a refusal now is a change from that rather than "
                    "a bike without the feature.";
      co_return read;
    }
    if (record.size() != 2) {
      read.reason = std::string(entry.field) + " answered " + std::to_string(record.size()) + " byte(s) (" +
                    toHex(record) + "), not the 2 a WORD should be";
      co_return read;
    }
    *fields[at++] = (record[0] << 8) | record[1];
  }
  read.ok = true;
  read.stamp = interpretServiceStamp(raw, nowMs);
  co_return read;
}

async::Task<ServicePointOutcome> WriteSession::setServicePoint(double nowMs) {
  ServicePointOutcome outcome;
  const Micro micro = routineMicroFor(ServiceRoutineName::kSetServicePoint);
  const double cooldown = cooldown_.remainingMs(micro, this->nowMs());
  if (cooldown > 0) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = "session";
    outcome.reason = std::string(microName(micro)) + " is still unlocked — wait " +
                     std::to_string(static_cast<long long>(std::ceil(cooldown / 1000))) + " s";
    co_return outcome;
  }
  if (!(co_await openSession(micro))) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = "session";
    outcome.reason = std::string(microName(micro)) + " did not answer 10 81";
    co_return outcome;
  }
  const StampRead beforeRead = co_await readStampInSession(nowMs);
  std::optional<ServiceStamp> before = beforeRead.ok ? std::optional<ServiceStamp>(beforeRead.stamp) : std::nullopt;
  if (!beforeRead.ok) {
    logWarning(kTag, "could not read the service stamp before setting it — %s", beforeRead.reason.c_str());
  }
  const UnlockResult unlocked = co_await unlock(micro);
  if (unlocked.kind == UnlockResult::Kind::kRefused) {
    outcome.status = ServicePointOutcome::Status::kRefused;
    outcome.stage = writeStageName(unlocked.stage);
    outcome.negativeResponseCode = unlocked.negativeResponseCode;
    outcome.description = unlocked.description;
    co_return outcome;
  }
  if (unlocked.kind == UnlockResult::Kind::kFailed) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = writeStageName(unlocked.stage);
    outcome.reason = unlocked.reason;
    co_return outcome;
  }
  VcuWriteRequest request;
  request.kind = VcuWriteRequest::Kind::kStartRoutine;
  request.routine = ServiceRoutineName::kSetServicePoint;
  const ExchangeResult started = co_await exchangeWrite(micro, request);
  if (started.kind != ExchangeResult::Kind::kReply) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = "routine";
    outcome.reason = describeExchangeFailure(started.kind == ExchangeResult::Kind::kTimeout, started.reason);
    co_return outcome;
  }
  if (started.frame.kind != VcuFrame::Kind::kPayload) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = "routine";
    outcome.reason = std::string("the reply was a ") + frameKindName(started.frame.kind) + " frame";
    co_return outcome;
  }
  const RoutineReply reply = decodeRoutineReply(started.frame.payload, ServiceRoutineName::kSetServicePoint);
  if (reply.kind == RoutineReply::Kind::kRefused) {
    outcome.status = ServicePointOutcome::Status::kRefused;
    outcome.stage = "routine";
    outcome.negativeResponseCode = reply.negativeResponseCode;
    outcome.description = reply.description;
    co_return outcome;
  }
  if (reply.kind != RoutineReply::Kind::kStarted) {
    outcome.status = ServicePointOutcome::Status::kFailed;
    outcome.stage = "routine";
    if (reply.kind == RoutineReply::Kind::kRoutineMismatch) {
      outcome.reason = "the micro echoed routine " + hex(reply.received) + ", not " +
                       hex(routineIdFor(ServiceRoutineName::kSetServicePoint)) + " — we do not know what ran";
    } else {
      outcome.reason = reply.reason +
                       ". The positive response to 31 FC was never logged by anyone, so an unexpected shape means the "
                       "outcome is unknown, not that it failed.";
    }
    co_return outcome;
  }
  const StampRead afterRead = co_await readStampInSession(nowMs);
  outcome.status = ServicePointOutcome::Status::kStarted;
  outcome.before = before;
  outcome.after = afterRead.ok ? std::optional<ServiceStamp>(afterRead.stamp) : std::nullopt;
  co_return outcome;
}

async::Task<ClearDtcsOutcome> WriteSession::clearStoredDtcs() {
  ClearDtcsOutcome outcome;
  const ExchangeResult result =
      co_await exchangeRaw(kObdFunctionalRequestId, std::nullopt, buildClearDtcsFrame(), true);
  if (result.kind != ExchangeResult::Kind::kReply) {
    outcome.status = ClearDtcsOutcome::Status::kFailed;
    outcome.reason = describeExchangeFailure(result.kind == ExchangeResult::Kind::kTimeout, result.reason);
    co_return outcome;
  }
  const ClearDtcsReply reply = decodeClearDtcsReply(result.frame.payload);
  switch (reply.kind) {
    case ClearDtcsReply::Kind::kCleared:
      outcome.status = ClearDtcsOutcome::Status::kCleared;
      co_return outcome;
    case ClearDtcsReply::Kind::kRefused:
      outcome.status = ClearDtcsOutcome::Status::kRefused;
      outcome.negativeResponseCode = reply.negativeResponseCode;
      outcome.description = reply.description;
      co_return outcome;
    case ClearDtcsReply::Kind::kUnrecognised:
      outcome.status = ClearDtcsOutcome::Status::kFailed;
      outcome.reason = reply.reason;
      co_return outcome;
  }
  co_return outcome;
}

async::Task<ResetVcuOutcome> WriteSession::resetVcu() {
  ResetVcuOutcome outcome;
  static const Micro order[] = {Micro::kA9, Micro::kA8};
  for (const Micro micro : order) {
    if (!(co_await openSession(micro))) {
      outcome.status = ResetVcuOutcome::Status::kFailed;
      outcome.stage = "session";
      outcome.micro = microName(micro);
      outcome.reason = std::string(microName(micro)) + " did not answer 10 81 — is the key ON?";
      co_return outcome;
    }
  }
  std::vector<std::string> notes;
  for (const Micro micro : order) {
    std::array<std::uint8_t, 8> frame{};
    frame[0] = targetAddress(micro);
    frame[1] = 2;
    frame[2] = kServiceEcuReset;
    frame[3] = kResetKeyOffOn;
    const ExchangeResult result = co_await exchangeRaw(kKwpRequestCanId, kKwpResponseCanId, frame, false);
    if (result.kind == ExchangeResult::Kind::kTimeout) {
      notes.push_back(std::string(microName(micro)) + ": issued (no reply — the micro is rebooting)");
      continue;
    }
    if (result.kind == ExchangeResult::Kind::kNotSent) {
      outcome.status = ResetVcuOutcome::Status::kFailed;
      outcome.stage = "reset";
      outcome.micro = microName(micro);
      outcome.reason = "the frame never reached the bus — " + result.reason;
      co_return outcome;
    }
    if (result.frame.kind != VcuFrame::Kind::kPayload) {
      outcome.status = ResetVcuOutcome::Status::kFailed;
      outcome.stage = "reset";
      outcome.micro = microName(micro);
      outcome.reason = std::string("the reply was a ") + frameKindName(result.frame.kind) + " frame";
      co_return outcome;
    }
    const std::vector<std::uint8_t>& payload = result.frame.payload;
    if (payload.size() >= 3 && payload[0] == kNegativeResponseService) {
      outcome.status = ResetVcuOutcome::Status::kRefused;
      outcome.micro = microName(micro);
      outcome.negativeResponseCode = payload[2];
      outcome.description = describeResetNrc(payload[2]);
      co_return outcome;
    }
    if (!payload.empty() && payload[0] == kPositiveResetResponse) {
      notes.push_back(std::string(microName(micro)) + ": accepted (positive 51)");
      continue;
    }
    outcome.status = ResetVcuOutcome::Status::kFailed;
    outcome.stage = "reset";
    outcome.micro = microName(micro);
    outcome.reason = "unexpected reply " + toHex(payload);
    co_return outcome;
  }
  outcome.status = ResetVcuOutcome::Status::kReset;
  std::string note;
  for (std::size_t at = 0; at < notes.size(); at++) {
    if (at > 0) note += "; ";
    note += notes[at];
  }
  outcome.note = note;
  co_return outcome;
}

SyncClockOutcome syncBikeClock(const VcuSend& send, double nowMs) {
  SyncClockOutcome outcome;
  const std::optional<std::array<std::uint8_t, 8>> frame = buildRtcSyncFrame(nowMs);
  if (!frame) {
    outcome.reason = "vcu-write: " + isoFromEpochMs(nowMs) +
                     " is outside the 2000…2255 this frame's year byte can express";
    return outcome;
  }
  if (const std::optional<std::string> problem = send(kRtcSyncCanId, *frame)) {
    outcome.reason = *problem;
    return outcome;
  }
  outcome.sent = true;
  outcome.hex = toHex(*frame);
  return outcome;
}

}  // namespace emcool::vcu
