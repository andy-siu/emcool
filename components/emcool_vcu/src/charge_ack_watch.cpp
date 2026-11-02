#include "emcool/vcu/charge_ack_watch.hpp"

#include <algorithm>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"

namespace emcool::vcu {

namespace {
constexpr const char* kTag = "charge-ack";
constexpr double kChargeManagerStateAc = 0x02;
constexpr double kChargeManagerStateDc = 0x23;
}  // namespace

void ChargeAckWatch::remember(ChargeMode mode, double amps, std::int64_t nowMs) {
  samples_.push_back({mode, {nowMs, amps}});
  const std::int64_t oldest = nowMs - (kAckSampleHistoryMs + kAckTimeoutMs);
  while (!samples_.empty() && samples_.front().sample.atMs < oldest) samples_.erase(samples_.begin());
  if (mode == ChargeMode::kDc) {
    lastDcTarget_ = amps;
  } else {
    lastAcLimit_ = amps;
  }
}

std::vector<AckSample> ChargeAckWatch::samplesFor(const Pending& command) const {
  std::vector<AckSample> forSignal;
  for (const Stamped& entry : samples_) {
    if (entry.mode == command.mode && entry.sample.atMs >= command.sentAtMs - kAckSampleHistoryMs) {
      forSignal.push_back(entry.sample);
    }
  }
  if (forSignal.empty()) {
    const std::optional<double> current = command.mode == ChargeMode::kDc ? lastDcTarget_ : lastAcLimit_;
    if (current) forSignal.push_back({command.sentAtMs, *current});
  }
  return forSignal;
}

void ChargeAckWatch::settle(const Pending& command, bool hasSuperseded, std::int64_t supersededAtMs,
                            std::int64_t nowMs) {
  ChargeAckInput input;
  input.commandedAmps = command.amps;
  input.sentAtMs = command.sentAtMs;
  input.nowMs = hasSuperseded ? supersededAtMs : nowMs;
  input.samples = samplesFor(command);
  input.hasSuperseded = hasSuperseded;
  input.supersededAtMs = supersededAtMs;
  const ChargeAckVerdict verdict = judgeChargeCommand(input);
  latest_ = ChargeAckState{verdict, describeChargeAck(verdict, command.amps)};
  recorder_.record("charge_cmd_ack", chargeAckCode(verdict.kind));
  settles_ = (settles_ + 1) & 0xff;
  recorder_.record("charge_cmd_ack_seq", settles_);
  if (verdict.kind == ChargeAckVerdict::Kind::kTook) recorder_.record("charge_cmd_ack_ms", static_cast<double>(verdict.latencyMs));
  logWarning(kTag, "%s %s A — %s", chargeModeNameUpper(command.mode), text::jsNumber(command.amps).c_str(),
             latest_->message.c_str());
  pending_.reset();
}

void ChargeAckWatch::forget() {
  pending_.reset();
  latest_.reset();
  samples_.clear();
}

void ChargeAckWatch::noteSent(ChargeMode mode, double amps, std::int64_t nowMs) {
  if (pending_) settle(*pending_, false, 0, nowMs);
  pending_ = Pending{mode, amps, nowMs};
  recorder_.record("charge_cmd_a", amps);
}

void ChargeAckWatch::observeAckSignal(ChargeMode mode, double amps, std::int64_t nowMs) {
  remember(mode, amps, nowMs);
  if (pending_ && nowMs >= pending_->sentAtMs + kAckTimeoutMs) settle(*pending_, false, 0, nowMs);
}

void ChargeAckWatch::observeSessionState(double state, std::int64_t nowMs) {
  if (state != kChargeManagerStateAc && state != kChargeManagerStateDc) {
    forget();
    return;
  }
  if (pending_ && nowMs >= pending_->sentAtMs + kAckTimeoutMs) settle(*pending_, false, 0, nowMs);
}

std::optional<ChargeAckState> ChargeAckWatch::state(std::int64_t nowMs) const {
  if (!pending_) return latest_;
  ChargeAckInput input;
  input.commandedAmps = pending_->amps;
  input.sentAtMs = pending_->sentAtMs;
  input.nowMs = nowMs;
  input.samples = samplesFor(*pending_);
  const ChargeAckVerdict verdict = judgeChargeCommand(input);
  return ChargeAckState{verdict, describeChargeAck(verdict, pending_->amps)};
}

}  // namespace emcool::vcu
