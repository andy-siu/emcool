#pragma once

// What the OBD-side state machines need from the firmware: a way to put a frame on the bus, a
// way to record a signal, and the time. Transport-free, so the host checks drive the same code
// the bike runs, on a scripted clock.
//
// cool-eva's versions are async functions on Node timers; here each is a state machine the
// decode task advances: frames in with handleFrame(), time in with tick(), and nextDeadline()
// for when it next needs a tick.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace emcool::obd {

// Every request emcool sends is a full 8-byte classic frame, as cool-eva's are.
using Payload = std::array<std::uint8_t, 8>;

class CanTransmitter {
 public:
  virtual ~CanTransmitter() = default;
  // Queues one standard-id frame. nullopt once it's queued; otherwise why it couldn't be —
  // where cool-eva's channel.send() threw.
  virtual std::optional<std::string> transmit(std::uint32_t id, const Payload& data) = 0;
};

class SignalRecorder {
 public:
  virtual ~SignalRecorder() = default;
  // cool-eva record(key, value): stamped now, on both clocks.
  virtual void record(std::string_view key, double value) = 0;
};

// The two clocks every step needs: monotonic for timeouts and ages, wall for what's shown.
struct Now {
  std::int64_t monotonicMs;
  std::int64_t wallClockMs;
};

// When a state machine next needs a tick. Deadlines in the same millisecond fire in the order
// they were set, as Node runs cool-eva's timers: a hold's wait running out and the poller's
// next check can land together, and which goes first decides whether the hold is granted.
struct Deadline {
  std::int64_t atMs;
  std::uint64_t order;
};

// Hands out the order; one per ObdStack, shared by its machines.
class DeadlineOrder {
 public:
  Deadline at(std::int64_t atMs) { return {atMs, ++last_}; }

 private:
  std::uint64_t last_ = 0;
};

inline bool before(const Deadline& left, const Deadline& right) {
  return left.atMs < right.atMs || (left.atMs == right.atMs && left.order < right.order);
}

// The earlier of two optional deadlines.
inline std::optional<Deadline> earliest(std::optional<Deadline> left, std::optional<Deadline> right) {
  if (!left) return right;
  if (!right) return left;
  return before(*left, *right) ? left : right;
}

}  // namespace emcool::obd
