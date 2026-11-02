#pragma once

// A small event loop with Node's ordering rules, so cool-eva's async TypeScript can be ported
// as C++20 coroutines that read like the original and run in the same order:
//   - timers fire in order of due time, ties in the order they were set
//   - a delay under 1 ms is 1 ms (Node's setTimeout rule)
//   - after each timer, and after each frame or request handled, every microtask runs before
//     anything else — so a resumed coroutine finishes its synchronous stretch first, as an
//     awaited promise's continuation does
//
// Time is whatever the owner says it is: the firmware feeds the monotonic clock, the host checks
// a scripted one. One loop per task; nothing here is thread-safe.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "emcool/obd/bus.hpp"

namespace emcool::async {

using Callback = std::function<void()>;

struct TimerId {
  std::uint64_t order = 0;
  bool valid() const { return order != 0; }
};

class Loop {
 public:
  // Timers share `order` with anything else whose deadlines must interleave with these (the OBD
  // side's), so ties across the two still go in the order they were set.
  explicit Loop(obd::DeadlineOrder& order) : order_(order) {}

  std::int64_t now() const { return nowMs_; }

  TimerId setTimeout(std::int64_t delayMs, Callback callback);
  // Every `intervalMs`, each time set again after it runs, as Node's setInterval; cleared with
  // clearTimeout, the same id throughout.
  TimerId setInterval(std::int64_t intervalMs, Callback callback);
  void clearTimeout(TimerId timer);
  void queueMicrotask(Callback callback);

  // The earliest live timer.
  std::optional<obd::Deadline> nextTimer() const;
  // Moves the clock (never backwards) and fires the earliest timer if it's due by then, with its
  // microtasks; false if none was due.
  bool fireNext(std::int64_t nowMs);
  // Moves the clock and fires every timer due by then, in order.
  void advanceTo(std::int64_t nowMs);
  // Runs `callback` now as a macrotask — a frame arriving, a request — then its microtasks.
  void run(std::int64_t nowMs, const Callback& callback);
  void drainMicrotasks();

 private:
  struct Timer {
    obd::Deadline due;
    Callback callback;
    std::int64_t intervalMs = 0;  // 0 for a timeout
    std::uint64_t id = 0;
  };
  void insert(Timer timer);

  void setNow(std::int64_t nowMs);

  obd::DeadlineOrder& order_;
  std::int64_t nowMs_ = 0;
  // Kept sorted by due time and order; a handful at a time, so a vector is plenty.
  std::vector<Timer> timers_;
  std::deque<Callback> microtasks_;
  // The interval whose callback is running, and whether it cleared itself, so it isn't set again.
  std::uint64_t runningInterval_ = 0;
  bool runningIntervalCleared_ = false;
};

}  // namespace emcool::async
