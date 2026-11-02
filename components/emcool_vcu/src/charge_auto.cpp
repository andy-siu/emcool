#include "emcool/vcu/charge_auto.hpp"

#include <algorithm>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"

namespace emcool::vcu {

namespace {
constexpr const char* kTag = "charge-auto";
}

ChargeAutomatic::ChargeAutomatic(bool enabled, ChargeCommandSink sink, ChargeAutoReaders readers,
                                 obd::SignalRecorder& recorder)
    : enabled_(enabled), sink_(std::move(sink)), readers_(std::move(readers)), recorder_(recorder) {
  if (!enabled_) {
    mode_ = ChargeAutoMode::kOff;
    reason_ = ChargeAutoReason::kDisabled;
    return;
  }
  firstSocMayNotBeACrossing_ = !isSocPlausible(readers_.latestValue("soc"));
}

void ChargeAutomatic::start() {
  if (started_) return;
  started_ = true;
  if (!enabled_) {
    recorder_.record("charge_auto_mode", chargeAutoModeCode(ChargeAutoMode::kOff));
    recorder_.record("charge_auto_reason", static_cast<double>(ChargeAutoReason::kDisabled));
    return;
  }
  publishMode();
  auto self = this;
  timer_ = async::currentLoop().setInterval(kAutoTickMs, [self] { self->runTick().detach(); });
}

void ChargeAutomatic::stop() {
  if (timer_.valid()) async::currentLoop().clearTimeout(timer_);
  timer_ = {};
}

ChargeAutoState ChargeAutomatic::state() const {
  return {mode_, reason_, lastCommand_ ? std::optional<int>(lastCommand_->first) : std::nullopt};
}

void ChargeAutomatic::setMode(ChargeAutoMode mode) {
  if (!enabled_) {
    logWarning(kTag, "ignoring a mode change — CHARGE_AUTO_ENABLED is 0");
    return;
  }
  mode_ = mode;
  if (mode == ChargeAutoMode::kAutomatic) riderOverride_ = false;
  publishMode();
  setReason(decide().reason);
  logWarning(kTag, "mode set to %s", mode == ChargeAutoMode::kAutomatic ? "automatic" : "off");
}

void ChargeAutomatic::noteChargeCurrentOutgoing(int amps, bool manual) {
  lastSentAmps_ = amps;
  if (manual) {
    standDown();
    logWarning(kTag, "a charge current was set by hand — standing down for this charge");
  }
}

void ChargeAutomatic::observeTemperature(double celsius) { rememberTemperature(celsius); }
void ChargeAutomatic::observeSoc(double percent) { rememberSoc(percent); }

void ChargeAutomatic::observeSelectedCurrent(double amps) {
  if (isRiderSetpoint(amps)) standDown();
}

void ChargeAutomatic::observeSessionState(double state) {
  if (!lastSessionState_ || state != *lastSessionState_) {
    lastSessionState_ = state;
    forgetSession();
  }
}

async::Task<> ChargeAutomatic::runTick() {
  if (inFlight_) co_return;
  const ChargeAutoDecision decision = decide();
  setReason(decision.reason);
  if (decision.kind != ChargeAutoDecision::Kind::kCommand) co_return;
  inFlight_ = true;
  const ChargeCommandResult outcome = co_await sink_(decision.amps);
  if (outcome.succeeded) {
    lastCommand_ = {decision.amps, readers_.nowMs()};
    recorder_.record("charge_auto_target_a", decision.amps);
  } else {
    logWarning(kTag, "%d A refused — %s", decision.amps, outcome.message.c_str());
  }
  inFlight_ = false;
  co_return;
}

ChargeAutoDecision ChargeAutomatic::decide() const {
  const std::optional<double> packTemperatureC = readers_.latestValue("batt_temp_hi");
  ChargeAutoInput input;
  input.enabled = mode_ == ChargeAutoMode::kAutomatic;
  input.packTemperatureC = packTemperatureC;
  input.packTemperatureAgeMs = readers_.ageMs("batt_temp_hi");
  input.packTemperaturePlausible = isPackTemperaturePlausible(packTemperatureC);
  input.chargeManagerState = readers_.latestValue("charge_manager_state");
  input.chargeManagerStateAgeMs = readers_.ageMs("charge_manager_state");
  input.ceilingAmps = readers_.latestValue("fast_dc_limit_max_a");
  input.commandedAmps = lastCommand_ ? std::optional<double>(lastCommand_->first) : std::nullopt;
  input.riderOverride = riderOverride_;
  input.samples = samples_;
  input.socPercent = readers_.latestValue("soc");
  input.socAgeMs = readers_.ageMs("soc");
  input.socSamples = socSamples_;
  input.requestedAmps = readers_.latestValue("fast_dc_target_a");
  input.lastCommandAtMs = lastCommand_ ? std::optional<std::int64_t>(lastCommand_->second) : std::nullopt;
  input.nowMs = readers_.nowMs();
  return decideChargeCurrent(input);
}

void ChargeAutomatic::rememberTemperature(double celsius) {
  if (!isPackTemperaturePlausible(celsius)) {
    logWarning(kTag, "ignoring an implausible pack temperature of %s °C", text::jsNumber(celsius).c_str());
    return;
  }
  const std::int64_t atMs = readers_.nowMs();
  samples_.push_back({atMs, celsius});
  const std::int64_t oldest = atMs - kRateWindowMs;
  while (samples_.size() > 1 && samples_[1].atMs < oldest) samples_.erase(samples_.begin());
}

void ChargeAutomatic::rememberSoc(double percent) {
  if (!isSocPlausible(percent)) {
    logWarning(kTag, "ignoring an implausible SOC of %s %%", text::jsNumber(percent).c_str());
    return;
  }
  if (firstSocMayNotBeACrossing_) {
    firstSocMayNotBeACrossing_ = false;
    logWarning(kTag,
               "keeping a first SOC of %s %% with no crossing instant behind it — no usable SOC had been recorded "
               "when this controller subscribed, so the first rate this session reports may over-state by up to one "
               "whole point",
               text::jsNumber(percent).c_str());
  }
  const std::int64_t atMs = readers_.nowMs();
  socSamples_.push_back({atMs, percent});
  const std::int64_t oldest = atMs - kSocWindowMs;
  while (!socSamples_.empty() && socSamples_.front().atMs < oldest) socSamples_.erase(socSamples_.begin());
}

void ChargeAutomatic::forgetSession() {
  lastCommand_.reset();
  lastSentAmps_.reset();
  riderOverride_ = false;
  samples_.clear();
  socSamples_.clear();
  setReason(decide().reason);
}

bool ChargeAutomatic::isRiderSetpoint(double observedAmps) const {
  if (lastSentAmps_ && observedAmps == *lastSentAmps_) return false;
  const std::optional<double> ceiling = readers_.latestValue("fast_dc_limit_max_a");
  if (ceiling && observedAmps == *ceiling) return false;
  return true;
}

void ChargeAutomatic::publishMode() { recorder_.record("charge_auto_mode", chargeAutoModeCode(state().mode)); }

void ChargeAutomatic::standDown() {
  riderOverride_ = true;
  setReason(ChargeAutoReason::kRider);
}

void ChargeAutomatic::setReason(ChargeAutoReason reason) {
  reason_ = reason;
  recorder_.record("charge_auto_reason", static_cast<double>(reason));
}

}  // namespace emcool::vcu
