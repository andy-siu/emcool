#include "emcool/obd/obd_hold.hpp"

#include <utility>

#include "emcool/log.hpp"

namespace emcool::obd {

namespace {

constexpr const char* kTag = "obd";

}  // namespace

std::optional<HoldTicket> ObdHold::request(std::string_view reason, std::int64_t nowMs, std::int64_t waitMs,
                                           std::int64_t maxHoldMs, Settled settled) {
  const std::string asking(reason);
  if (active_) {
    logWarning(kTag, "refusing to park for %s — %s already has it", asking.c_str(), reason_.c_str());
    return std::nullopt;
  }
  active_ = true;
  ticket_ = ++nextTicket_;
  reason_ = asking;
  parked_ = false;
  waitMs_ = waitMs;
  waitUntil_ = order_.at(nowMs + waitMs);
  expiresAtMs_ = 0;
  maxHoldMs_ = maxHoldMs;
  settled_ = std::move(settled);
  return ticket_;
}

HoldStatus ObdHold::status(HoldTicket ticket) const {
  if (active_ && ticket == ticket_) return parked_ ? HoldStatus::kParked : HoldStatus::kWaiting;
  if (ticket == refused_) return HoldStatus::kRefused;
  return HoldStatus::kEnded;
}

void ObdHold::release(HoldTicket ticket) {
  if (!active_ || ticket != ticket_) return;
  active_ = false;
  // Before the poller parked this withdraws the request, which never stopped anything.
  if (parked_) logInfo(kTag, "resumed after %s", reason_.c_str());
}

bool ObdHold::parkedForHold(std::int64_t nowMs) {
  if (!active_) return false;
  if (parked_ && expiresAtMs_ > 0 && nowMs > expiresAtMs_) {
    logWarning(kTag, "%s held the poller past %lld ms — resuming anyway", reason_.c_str(),
               static_cast<long long>(maxHoldMs_));
    active_ = false;
    return false;
  }
  if (!parked_) {
    parked_ = true;
    expiresAtMs_ = nowMs + maxHoldMs_;
    logInfo(kTag, "parked for %s", reason_.c_str());
    if (Settled settled = std::exchange(settled_, nullptr)) settled(true);
  }
  return true;
}

void ObdHold::tick(std::int64_t nowMs) {
  if (!active_ || parked_ || nowMs < waitUntil_.atMs) return;
  active_ = false;
  refused_ = ticket_;
  logWarning(kTag, "the poller did not park within %lld ms — refusing %s", static_cast<long long>(waitMs_),
             reason_.c_str());
  if (Settled settled = std::exchange(settled_, nullptr)) settled(false);
}

std::optional<Deadline> ObdHold::nextDeadline() const {
  // Parked, the cap is enforced at the poller's next check, which comes every 100 ms anyway.
  if (!active_ || parked_) return std::nullopt;
  return waitUntil_;
}

std::string pollerRefusalFor(std::string_view what) {
  return "the OBD poller would not go quiet in time, so " + std::string(what) +
         " could not have the bus to itself — nothing was sent";
}

}  // namespace emcool::obd
