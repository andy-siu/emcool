#pragma once

// The firmware's side of the dashboard's Settings → Bike console (ble_server/console_endpoints.hpp):
// the status report, written by the decode task that owns its counters; the `network` report; what
// stops a restart (a moving bike, the bus in use); and the restart, a moment after the reply.

#include <mutex>
#include <string>
#include <vector>

#include "diagnostics_task.hpp"
#include "emcool/ble_server/console_endpoints.hpp"
#include "emcool/signals/signal_store.hpp"
#include "status_report.hpp"

namespace emcool {

class DashboardConsoleHost final : public ble_server::ConsoleHost {
 public:
  DashboardConsoleHost(StatusText& status, const signals::SignalStore& store, std::mutex& storeMutex,
                       DiagnosticsTask& diagnostics)
      : status_(status), store_(store), storeMutex_(storeMutex), diagnostics_(diagnostics) {}

  bool statusReport(bool first, std::string& text) override;
  std::string network() override;
  std::vector<std::string> restartBlockers() override;
  void restartSoon() override;

 private:
  StatusText& status_;
  const signals::SignalStore& store_;
  std::mutex& storeMutex_;
  DiagnosticsTask& diagnostics_;
};

// Restarts after `delayMs`, from a task of its own, so the reply to whoever asked has time to go.
void restartAfter(int delayMs);

}  // namespace emcool
