#pragma once

// Coroutines for porting cool-eva's async functions, on emcool::async::Loop:
//
//   Task<T>      an async function. It starts at once and runs until its first co_await of
//                something not yet done, as a JS async function does. co_await on it resumes
//                the caller as a microtask once it returns. Keep the Task until it's done, or
//                detach() it to let it run on by itself (`void promise` in cool-eva).
//   Deferred<T>  a promise resolved by hand (`new Promise(resolve => …)`); any number of
//                coroutines may co_await it, and resolve() wakes them as microtasks.
//   sleep(ms)    `await new Promise(r => setTimeout(r, ms))`.
//
// Like a JS await, a co_await always yields at least once, even on something already done, so
// the order of side effects matches. Exceptions are off on the firmware, and nothing here throws.

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "emcool/async/loop.hpp"

namespace emcool::async {

// The loop coroutines schedule on: one per task (the diagnostics task's, or a host check's).
Loop& currentLoop();
void setCurrentLoop(Loop* loop);

namespace detail {

inline void resumeLater(std::coroutine_handle<> handle) {
  currentLoop().queueMicrotask([handle] { handle.resume(); });
}

struct PromiseBase {
  std::coroutine_handle<> continuation;
  bool detached = false;
  bool done = false;

  std::suspend_never initial_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept { std::terminate(); }

  struct Final {
    bool await_ready() noexcept { return false; }
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
      PromiseBase& promise = handle.promise();
      promise.done = true;
      if (promise.continuation) {
        resumeLater(promise.continuation);
      } else if (promise.detached) {
        handle.destroy();
      }
    }
    void await_resume() noexcept {}
  };
  Final final_suspend() noexcept { return {}; }
};

template <typename T>
struct ValuePromise : PromiseBase {
  std::optional<T> value;
  void return_value(T result) { value = std::move(result); }
  T take() { return std::move(*value); }
};

struct VoidPromise : PromiseBase {
  void return_void() {}
  void take() {}
};

}  // namespace detail

template <typename T = void>
class Task {
 public:
  struct promise_type : std::conditional_t<std::is_void_v<T>, detail::VoidPromise, detail::ValuePromise<T>> {
    Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
  };

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { release(); }

  bool done() const { return handle_ && handle_.promise().done; }
  // Lets it run on by itself; its frame goes when it returns.
  void detach() {
    if (!handle_) return;
    if (handle_.promise().done) {
      handle_.destroy();
    } else {
      handle_.promise().detached = true;
    }
    handle_ = nullptr;
  }

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> caller) noexcept {
    if (handle_.promise().done) {
      detail::resumeLater(caller);
    } else {
      handle_.promise().continuation = caller;
    }
  }
  decltype(auto) await_resume() { return handle_.promise().take(); }

 private:
  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  // A Task dropped before it's done keeps running, as an unawaited promise does.
  void release() {
    if (handle_) detach();
  }

  std::coroutine_handle<promise_type> handle_;
};

template <typename T>
class Deferred {
  struct State {
    std::optional<T> value;
    std::vector<std::coroutine_handle<>> waiters;
  };

 public:
  Deferred() : state_(std::make_shared<State>()) {}

  bool settled() const { return state_->value.has_value(); }
  // The first resolve wins, as a promise's does.
  void resolve(T value) {
    if (state_->value) return;
    state_->value = std::move(value);
    for (const std::coroutine_handle<> waiter : state_->waiters) detail::resumeLater(waiter);
    state_->waiters.clear();
  }

  struct Awaiter {
    std::shared_ptr<State> state;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> caller) {
      if (state->value) {
        detail::resumeLater(caller);
      } else {
        state->waiters.push_back(caller);
      }
    }
    T await_resume() { return *state->value; }
  };
  Awaiter operator co_await() const { return Awaiter{state_}; }

 private:
  std::shared_ptr<State> state_;
};

// `await sleep(ms)`: resumes from the timer itself, as the promise's continuation would run
// straight after the timer's callback.
struct Sleep {
  std::int64_t delayMs;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> caller) {
    currentLoop().setTimeout(delayMs, [caller] { caller.resume(); });
  }
  void await_resume() const noexcept {}
};
inline Sleep sleep(std::int64_t delayMs) { return {delayMs}; }

}  // namespace emcool::async
