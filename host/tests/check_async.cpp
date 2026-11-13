// The event loop and coroutines keep JavaScript's ordering, which the VCU ports depend on.
// Each case is a small reference-shaped program, with the order Node runs it in written out.

#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/async/task.hpp"

using namespace emcool::async;

namespace {

std::vector<std::string> trace;
void note(const std::string& line) { trace.push_back(line); }

std::string joined() {
  std::string text;
  for (const std::string& line : trace) text += (text.empty() ? "" : " ") + line;
  return text;
}

// async function work(ms) { note("work start"); await sleep(ms); note("work end"); return ms * 2; }
Task<int> work(std::int64_t ms) {
  note("work start");
  co_await sleep(ms);
  note("work end " + std::to_string(currentLoop().now()));
  co_return static_cast<int>(ms * 2);
}

Task<> eager() {
  note("main start");
  Task<int> pending = work(10);
  note("main after call");
  const int value = co_await pending;
  note("main got " + std::to_string(value));
}

Task<> waits(const Deferred<int>& deferred, std::string name) {
  const int value = co_await deferred;
  note(name + " " + std::to_string(value));
}

Task<int> immediate() { co_return 7; }

Task<> awaitsImmediate() {
  note("before");
  const int value = co_await immediate();
  note("after " + std::to_string(value));
}

Task<> sleeper(std::string name, std::int64_t ms) {
  co_await sleep(ms);
  note(name);
}

Task<int> nested(int depth) {
  if (depth == 0) {
    co_await sleep(5);
    co_return 1;
  }
  co_return 1 + co_await nested(depth - 1);
}

}  // namespace

int main() {
  emcool::obd::DeadlineOrder order;
  Loop loop(order);
  setCurrentLoop(&loop);

  // A called async function runs up to its first await before the caller carries on.
  trace.clear();
  loop.run(0, [] { eager().detach(); });
  loop.advanceTo(10);
  EMCOOL_CHECK(joined() == "main start work start main after call work end 10 main got 20", "eager: %s",
               joined().c_str());

  // Awaiting something already resolved still yields: the synchronous code after goes first.
  trace.clear();
  loop.run(20, [] {
    Deferred<int> ready;
    ready.resolve(1);
    waits(ready, "awaited").detach();
    note("synchronous");
  });
  EMCOOL_CHECK(joined() == "synchronous awaited 1", "already resolved: %s", joined().c_str());
  trace.clear();
  loop.run(21, [] { awaitsImmediate().detach(); note("caller"); });
  EMCOOL_CHECK(joined() == "before caller after 7", "an async function with no await: %s", joined().c_str());

  // resolve() wakes every waiter, in the order they waited, and only once.
  trace.clear();
  Deferred<int> shared;
  loop.run(30, [&] {
    waits(shared, "first").detach();
    waits(shared, "second").detach();
  });
  loop.run(31, [&] {
    shared.resolve(5);
    shared.resolve(6);
    note("resolver");
  });
  EMCOOL_CHECK(joined() == "resolver first 5 second 5", "waiters: %s", joined().c_str());

  // Timers due together fire in the order they were set; a delay of 0 is 1 ms.
  trace.clear();
  loop.run(40, [] {
    sleeper("b", 10).detach();
    sleeper("a", 10).detach();
    sleeper("zero", 0).detach();
  });
  loop.advanceTo(40);
  EMCOOL_CHECK(joined().empty(), "a 0 ms sleep fired in the same millisecond");
  loop.advanceTo(41);
  EMCOOL_CHECK(joined() == "zero", "a 0 ms sleep did not fire 1 ms later: %s", joined().c_str());
  loop.advanceTo(50);
  EMCOOL_CHECK(joined() == "zero b a", "same-millisecond timers: %s", joined().c_str());

  // A timer's microtasks run before the next timer, even one due in the same millisecond.
  trace.clear();
  Deferred<int> handoff;
  loop.run(60, [&] {
    waits(handoff, "continuation").detach();
    loop.setTimeout(5, [&] {
      note("timer one");
      handoff.resolve(9);
    });
    loop.setTimeout(5, [] { note("timer two"); });
  });
  loop.advanceTo(65);
  EMCOOL_CHECK(joined() == "timer one continuation 9 timer two", "microtasks between timers: %s", joined().c_str());

  // Cleared timers don't fire; nested tasks unwind with their values.
  trace.clear();
  bool fired = false;
  loop.run(70, [&] { loop.clearTimeout(loop.setTimeout(3, [&] { fired = true; })); });
  loop.advanceTo(80);
  EMCOOL_CHECK(!fired && !loop.nextTimer(), "a cleared timer fired");
  int depth = 0;
  loop.run(80, [&] {
    [](int& out) -> Task<> { out = co_await nested(20); }(depth).detach();
  });
  loop.advanceTo(85);
  EMCOOL_CHECK(depth == 21, "nested tasks returned %d", depth);

  // An interval fires every period, set again from when it ran, until it clears itself.
  trace.clear();
  int ticks = 0;
  TimerId interval;
  loop.run(100, [&] {
    interval = loop.setInterval(200, [&] {
      ticks++;
      note("tick " + std::to_string(loop.now()));
      if (ticks == 3) loop.clearTimeout(interval);
    });
  });
  // Step to each due time, as a scripted clock does (advanceTo jumps the clock to its target first).
  while (const auto next = loop.nextTimer()) {
    if (next->atMs > 1000) break;
    loop.fireNext(next->atMs);
  }
  EMCOOL_CHECK(joined() == "tick 300 tick 500 tick 700" && !loop.nextTimer(), "interval: %s", joined().c_str());

  setCurrentLoop(nullptr);
  std::printf("check_async: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
