#include "emcool/async/loop.hpp"

#include <algorithm>
#include <cstdlib>

#include "emcool/async/task.hpp"

namespace emcool::async {

namespace {

Loop* current = nullptr;

}  // namespace

Loop& currentLoop() {
  if (current == nullptr) std::abort();  // a coroutine scheduled with no loop to run it
  return *current;
}

void setCurrentLoop(Loop* loop) { current = loop; }

void Loop::insert(Timer timer) {
  const auto at = std::upper_bound(timers_.begin(), timers_.end(), timer.due,
                                   [](const obd::Deadline& left, const Timer& right) { return obd::before(left, right.due); });
  timers_.insert(at, std::move(timer));
}

TimerId Loop::setTimeout(std::int64_t delayMs, Callback callback) {
  const obd::Deadline due = order_.at(nowMs_ + std::max<std::int64_t>(1, delayMs));
  insert(Timer{due, std::move(callback), 0, due.order});
  return {due.order};
}

TimerId Loop::setInterval(std::int64_t intervalMs, Callback callback) {
  const obd::Deadline due = order_.at(nowMs_ + std::max<std::int64_t>(1, intervalMs));
  insert(Timer{due, std::move(callback), std::max<std::int64_t>(1, intervalMs), due.order});
  return {due.order};
}

void Loop::clearTimeout(TimerId timer) {
  if (!timer.valid()) return;
  if (runningInterval_ == timer.order) runningIntervalCleared_ = true;
  const auto found =
      std::find_if(timers_.begin(), timers_.end(), [&](const Timer& candidate) { return candidate.id == timer.order; });
  if (found != timers_.end()) timers_.erase(found);
}

void Loop::queueMicrotask(Callback callback) { microtasks_.push_back(std::move(callback)); }

std::optional<obd::Deadline> Loop::nextTimer() const {
  if (timers_.empty()) return std::nullopt;
  return timers_.front().due;
}

void Loop::setNow(std::int64_t nowMs) { nowMs_ = std::max(nowMs_, nowMs); }

bool Loop::fireNext(std::int64_t nowMs) {
  setNow(nowMs);
  if (timers_.empty() || timers_.front().due.atMs > nowMs_) return false;
  Timer timer = std::move(timers_.front());
  timers_.erase(timers_.begin());
  if (timer.intervalMs > 0) {
    // Runs, then goes back in from now, unless it cleared itself while running.
    runningInterval_ = timer.id;
    runningIntervalCleared_ = false;
    timer.callback();
    runningInterval_ = 0;
    if (!runningIntervalCleared_) {
      timer.due = order_.at(nowMs_ + timer.intervalMs);
      insert(std::move(timer));
    }
  } else {
    timer.callback();
  }
  drainMicrotasks();
  return true;
}

void Loop::advanceTo(std::int64_t nowMs) {
  while (fireNext(nowMs)) {
  }
}

void Loop::run(std::int64_t nowMs, const Callback& callback) {
  setNow(nowMs);
  callback();
  drainMicrotasks();
}

void Loop::drainMicrotasks() {
  while (!microtasks_.empty()) {
    Callback callback = std::move(microtasks_.front());
    microtasks_.pop_front();
    callback();
  }
}

}  // namespace emcool::async
