#include "dashboard_console.hpp"

#include "emcool/vcu/service_gate.hpp"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_time.hpp"
#include "wifi_link.hpp"

namespace emcool {

bool DashboardConsoleHost::statusReport(bool first, std::string& text) {
  if (first) {
    status_.request();
    return false;
  }
  std::optional<std::string> report = status_.take();
  if (!report) return false;
  text = std::move(*report);
  return true;
}

std::string DashboardConsoleHost::network() { return describeNetwork(); }

std::vector<std::string> DashboardConsoleHost::restartBlockers() {
  const vcu::GateReadings readings = vcu::sampleServiceGate([this](std::string_view key) {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    vcu::GateSample sample;
    if (const std::optional<signals::LiveValue> live = store_.latest(key)) sample.value = live->value;
    if (const std::optional<std::int64_t> age = store_.ageMs(key, monotonicMs())) {
      sample.ageMs = static_cast<double>(*age);
    }
    return sample;
  });
  std::vector<std::string> blockers;
  for (const std::string& requirement : vcu::restartMotionBlockers(readings)) {
    blockers.push_back("wait until " + requirement);
  }
  const DiagnosticsSummary summary = diagnostics_.summary();
  if (!summary.busHeldBy.empty()) {
    blockers.push_back(summary.busHeldBy + " is using the bus");
  } else if (summary.sweepRunning) {
    blockers.emplace_back("a service-mode parameter read is running");
  }
  if (!summary.heldBy.empty()) blockers.push_back("the OBD poller is held for " + summary.heldBy);
  return blockers;
}

void DashboardConsoleHost::restartSoon() { restartAfter(1500); }

void restartAfter(int delayMs) {
  static int delay = 0;
  delay = delayMs;
  xTaskCreatePinnedToCore(
      [](void*) {
        vTaskDelay(pdMS_TO_TICKS(delay));
        esp_restart();
      },
      "restart", 2048, nullptr, 5, nullptr, 0);
}

}  // namespace emcool
