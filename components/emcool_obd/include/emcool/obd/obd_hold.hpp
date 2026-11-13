#pragma once

// Parking the OBD poller so something else can have the bus to itself. A service-mode read or
// write asks for the hold; the poller parks at its next check (between PID requests, or before
// a trouble-code read), and the asker sees kParked. Nothing waits for ever: the asker gives up
// if the poller hasn't parked within 6 s, and the poller takes the bus back after 15 s however
// the hold's owner is doing.
//
// One hold at a time; a second asker is refused at once.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "emcool/obd/bus.hpp"

namespace emcool::obd {

using HoldTicket = std::uint32_t;

enum class HoldStatus : std::uint8_t {
  kWaiting,  // asked; the poller hasn't parked yet
  kParked,   // the bus is the owner's until release() or the 15 s cap
  kRefused,  // the poller didn't park within the wait
  kEnded,    // released, or taken back at the cap
};

class ObdHold {
 public:
  static constexpr std::int64_t kMaxHoldMs = 15000;
  static constexpr std::int64_t kHoldWaitMs = 6000;

  explicit ObdHold(DeadlineOrder& order) : order_(order) {}

  // holdObdPoller(reason): nullopt, and a warning, if someone else has the hold. `settled`, if given,
  // is called once the poller parks (true) or the wait runs out (false).
  using Settled = std::function<void(bool parked)>;
  std::optional<HoldTicket> request(std::string_view reason, std::int64_t nowMs, std::int64_t waitMs = kHoldWaitMs,
                                    std::int64_t maxHoldMs = kMaxHoldMs, Settled settled = nullptr);
  HoldStatus status(HoldTicket ticket) const;
  // Only the ticket's own hold, and only while it lasts: a hold taken back at the cap can't
  // release whoever asked next. Before the poller has parked, it withdraws the request.
  void release(HoldTicket ticket);

  // The poller's check: true while it must stay off the bus. The first check after a request
  // is what parks it.
  bool parkedForHold(std::int64_t nowMs);
  // The asker's wait running out.
  void tick(std::int64_t nowMs);
  std::optional<Deadline> nextDeadline() const;

  // The reason, or empty when nobody holds it.
  std::string_view heldBy() const { return active_ ? std::string_view(reason_) : std::string_view(); }

 private:
  DeadlineOrder& order_;
  bool active_ = false;
  HoldTicket ticket_ = 0;
  HoldTicket refused_ = 0;
  HoldTicket nextTicket_ = 0;
  std::string reason_;
  bool parked_ = false;
  Deadline waitUntil_ = {0, 0};
  std::int64_t expiresAtMs_ = 0;
  std::int64_t maxHoldMs_ = kMaxHoldMs;
  std::int64_t waitMs_ = kHoldWaitMs;
  Settled settled_;
};

// What to tell the user when the hold was refused.
std::string pollerRefusalFor(std::string_view what);

}  // namespace emcool::obd
