#pragma once

// The serial console's status report: the CAN bus, the ride log, the clock, Bluetooth and Wi-Fi, and a
// few live values to recognise a healthy bike at a glance. Every 10 s by default; `status off`,
// `status on` and `status every SECONDS` on the console change that at once, and `status` asks
// for one now. The dashboard's Settings → Bike console reads the same lines (StatusText).

#include <cstdint>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "ble_link.hpp"
#include "can_bus.hpp"
#include "emcool/clock/system_clock_sync.hpp"
#include "diagnostics_task.hpp"
#include "emcool/ride_log/log_writer.hpp"
#include "emcool/ride_log/ride_log.hpp"
#include "emcool/settings/settings.hpp"
#include "emcool/signals/signal_store.hpp"
#include "ride_log_task.hpp"
#include "wifi_link.hpp"

namespace emcool {

// The report's lines for the dashboard, without printing them: asked for from the Bluetooth task,
// written by the decode task, which owns the counters the report reads.
class StatusText {
 public:
  void request() {
    const std::lock_guard<std::mutex> lock(mutex_);
    ready_ = false;
    requested_.store(true, std::memory_order_relaxed);
  }
  bool takeRequest() { return requested_.exchange(false, std::memory_order_relaxed); }
  void publish(std::string text) {
    const std::lock_guard<std::mutex> lock(mutex_);
    text_ = std::move(text);
    ready_ = true;
  }
  std::optional<std::string> take() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return std::nullopt;
    ready_ = false;
    return std::move(text_);
  }

 private:
  std::mutex mutex_;
  std::atomic<bool> requested_{false};
  bool ready_ = false;
  std::string text_;
};

struct StatusSources {
  const CanBus& bus;
  const signals::SignalStore& store;
  std::mutex& storeMutex;
  const ride_log::RideLog& rideLog;
  const ride_log::LogWriter& writer;
  const RideLogTaskCounters& rideLogTask;
  const clock::SystemClockSync& clock;
  const BleLinkCounters& bleLink;
  const WifiCounters& wifi;
  bool wifiOn;
  settings::RuntimeSettings& settings;
  DiagnosticsTask* diagnostics;
  StatusText& text;
};

class StatusReport {
 public:
  explicit StatusReport(const StatusSources& sources) : sources_(sources) {}

  void noteFrame(std::size_t valueCount, bool overflowed);
  // From the decode task: the periodic report, and the dashboard's text when it asked for it.
  void reportIfDue(std::int64_t nowMs);

 private:
  // The report's lines, each ending in a newline, over the window so far.
  std::string describe(std::int64_t nowMs);

  struct Totals {
    std::uint32_t received = 0;
    std::uint32_t dropped = 0;
    std::uint32_t busErrors = 0;
    std::uint64_t values = 0;
    std::uint64_t overflowedFrames = 0;
    std::uint64_t appended = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t framesNotified = 0;
    std::uint32_t sent = 0;
  };

  Totals current() const;

  const StatusSources sources_;
  std::uint64_t values_ = 0;
  std::uint64_t overflowedFrames_ = 0;
  std::int64_t windowStartedMs_ = -1;
  Totals windowStart_;
  settings::StatusSchedule schedule_;
};

}  // namespace emcool
