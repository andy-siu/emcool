#pragma once

// Service mode's endpoints, answered as cool-eva's src/http/ handlers answer them — past their
// method and header checks, which the Bluetooth server makes before a request gets here:
//   /vcu-read          GET the run, POST to start a sweep, DELETE to stop it
//   /vcu-probe         POST one identifier
//   /lifetime-read     POST: read the lifetime statistics, and keep them
//   /freeze-frame-read POST: read the stored freeze frames, and keep them
//   /lifetime-stats    GET the last lifetime reading
//   /freeze-frames     GET the last freeze-frame reading
//   /expected-faults   GET the list, POST to mark a code expected or not
// The reads run in the task that owns the bus, so an answer can take seconds: answer() is a
// coroutine.

#include <functional>
#include <optional>
#include <string>

#include "emcool/async/task.hpp"
#include "emcool/vcu/reading_stores.hpp"
#include "emcool/vcu/runner.hpp"
#include "emcool/vcu/store.hpp"
#include "emcool/vcu/write_runner.hpp"

namespace emcool::vcu {

struct ServiceRequest {
  enum class Kind : std::uint8_t {
    kReadState,
    kReadStart,
    kReadCancel,
    kProbe,
    kLifetimeRead,
    kFreezeFrameRead,
    kLifetimeStats,
    kFreezeFrames,
    kExpectedFaults,
    kSetExpectedFault,
    kWriteStatus,   // GET /vcu-write
    kWritePerform,  // POST /vcu-write
    kChargeAutoStatus,  // GET /charge-auto
    kChargeAutoSet,     // POST /charge-auto?mode=automatic|off
  };
  Kind kind;
  // From the query, as sent: a probe's target/bank/index, an expected fault's component/symptom/expected.
  std::optional<std::string> target, bank, index;
  std::optional<std::string> component, symptom, expected;
  // The whole /vcu-write query string, re-parsed by the API (action, detail, list, confirm, …).
  std::string query;
};

struct ServiceAnswer {
  int status;
  std::string body;  // JSON
};

class ServiceApi {
 public:
  ServiceApi(VcuReadRunner& runner, VcuStore& store, ReadingStore& readings, bool enabled,
             std::function<double()> wallClockMs, VcuWriteRunner* writeRunner = nullptr)
      : runner_(runner),
        store_(store),
        readings_(readings),
        enabled_(enabled),
        wallClockMs_(std::move(wallClockMs)),
        writeRunner_(writeRunner) {}

  async::Task<ServiceAnswer> answer(ServiceRequest request);

 private:
  std::string readBody(const std::optional<std::string>& message);

  VcuReadRunner& runner_;
  VcuStore& store_;
  ReadingStore& readings_;
  const bool enabled_;
  std::function<double()> wallClockMs_;
  VcuWriteRunner* writeRunner_;
};

// "service mode is switched off on this bike (SERVICE_MODE_ENABLED=0)".
extern const char* const kServiceModeDisabled;

}  // namespace emcool::vcu
