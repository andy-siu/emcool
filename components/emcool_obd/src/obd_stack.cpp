#include "emcool/obd/obd_stack.hpp"

#include "emcool/log.hpp"

namespace emcool::obd {

void ObdStack::start(Now now) {
  poller_.start(now);
}

void ObdStack::handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, Now now) {
  if (ObdPoller::isResponse(id)) poller_.handleResponse(id, data, now);
}

void ObdStack::tick(Now now) {
  // One at a time, re-reading the deadlines after each: firing one can set or clear others.
  // Each firing moves its own deadline on, so this ends; the cap is in case one ever didn't.
  for (int fired = 0; fired < 64; fired++) {
    if (!fireNext(now)) return;
  }
  logError("obd", "a deadline kept firing without moving on; leaving it until the next tick");
}

std::optional<Deadline> ObdStack::nextDeadline() const {
  return earliest(earliest(hold_.nextDeadline(), reader_.nextDeadline()), poller_.nextDeadline());
}

bool ObdStack::fireNext(Now now) {
  const std::optional<Deadline> hold = hold_.nextDeadline();
  const std::optional<Deadline> reader = reader_.nextDeadline();
  const std::optional<Deadline> next = nextDeadline();
  if (!next || next->atMs > now.monotonicMs) return false;
  if (hold && hold->order == next->order) {
    hold_.tick(now.monotonicMs);
  } else if (reader && reader->order == next->order) {
    reader_.tick(now);
    poller_.takeReadOutcome(now);
  } else {
    poller_.tick(now);
  }
  return true;
}

std::optional<std::int64_t> ObdStack::nextDeadlineMs() const {
  const std::optional<Deadline> next = nextDeadline();
  if (!next) return std::nullopt;
  return next->atMs;
}

}  // namespace emcool::obd
