#include "status_report.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "emcool/log.hpp"
#include "esp_heap_caps.h"

namespace emcool {

namespace {

constexpr const char* kTag = "emcool";

// Position is deliberately not printed.
constexpr const char* kReportedSignals[] = {
    "soc", "pack_v", "pack_a", "speed_can_kmh", "vehicle_state_can", "key_on", "gps_satellites",
};

}  // namespace

void StatusReport::noteFrame(std::size_t valueCount, bool overflowed) {
  values_ += valueCount;
  if (overflowed) {
    overflowedFrames_++;
  }
}

StatusReport::Totals StatusReport::current() const {
  const CanBusCounters& bus = sources_.bus.counters();
  Totals totals;
  totals.received = bus.received.load(std::memory_order_relaxed);
  totals.dropped = bus.droppedQueueFull.load(std::memory_order_relaxed);
  totals.busErrors = bus.busErrors.load(std::memory_order_relaxed);
  totals.values = values_;
  totals.overflowedFrames = overflowedFrames_;
  totals.appended = sources_.rideLog.counters().readingsAppended;
  totals.bytesWritten = sources_.writer.counters().bytesWritten.load(std::memory_order_relaxed);
  totals.framesNotified = sources_.bleLink.framesSent.load(std::memory_order_relaxed);
  totals.sent = bus.sent.load(std::memory_order_relaxed);
  return totals;
}

void StatusReport::reportIfDue(std::int64_t nowMs) {
  const settings::StatusSchedule::Decision decision =
      schedule_.decide(sources_.settings, sources_.settings.takeReportRequest(), nowMs);
  if (decision.report && windowStartedMs_ >= 0) {
    report(nowMs);
  }
  if (decision.restartWindow) {
    windowStartedMs_ = nowMs;
    windowStart_ = current();
  }
}

void StatusReport::report(std::int64_t nowMs) {
  const Totals now = current();
  // A `status` straight after a window began covers very little time; never divide by none.
  const double seconds = std::max(static_cast<double>(nowMs - windowStartedMs_), 1.0) / 1000.0;
  const CanBusStatus status = sources_.bus.status();
  std::uint32_t unknownKeys = 0;
  {
    const std::lock_guard<std::mutex> lock(sources_.storeMutex);
    unknownKeys = sources_.store.unknownKeyCount();
  }

  logInfo(kTag,
          "can: %.0f frames/s, %" PRIu32 " dropped, %" PRIu32 " bus errors, %s (rx errors %u) | %.0f values/s, "
          "%" PRIu32 " unknown keys, %llu overflowed frames | heap %u free, %u lowest",
          (now.received - windowStart_.received) / seconds, now.dropped - windowStart_.dropped,
          now.busErrors - windowStart_.busErrors, status.state, status.rxErrorCount,
          static_cast<double>(now.values - windowStart_.values) / seconds, unknownKeys,
          static_cast<unsigned long long>(now.overflowedFrames - windowStart_.overflowedFrames),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));

  const ride_log::RideLogCounters& rideLog = sources_.rideLog.counters();
  const ride_log::LogWriterCounters& writer = sources_.writer.counters();
  logInfo(kTag,
          "ride log: card %s, boot %05u part %u | %.0f readings/s, %.1f KB/s to the card | %llu dropped, %u lost "
          "without a card, %llu write errors, %u old parts deleted for room | clock %s",
          sources_.rideLogTask.cardMounted.load(std::memory_order_relaxed) ? "mounted" : "ABSENT",
          static_cast<unsigned>(writer.bootNumber.load(std::memory_order_relaxed)),
          static_cast<unsigned>(writer.currentPart.load(std::memory_order_relaxed)),
          static_cast<double>(now.appended - windowStart_.appended) / seconds,
          static_cast<double>(now.bytesWritten - windowStart_.bytesWritten) / 1024.0 / seconds,
          static_cast<unsigned long long>(rideLog.readingsDropped),
          static_cast<unsigned>(sources_.rideLogTask.readingsLost.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(writer.writeErrors.load(std::memory_order_relaxed)),
          static_cast<unsigned>(writer.partsRotated.load(std::memory_order_relaxed)),
          clock::clockTrustName(sources_.clock.trust()));

  if (!sources_.bluetooth) {
    logInfo(kTag, "bluetooth: off");
  } else {
    const BleLinkCounters& link = sources_.bleLink;
    logInfo(kTag, "bluetooth: %s, MTU %u | %.0f notifications/s | %u writes dropped, %u notify failures",
            !link.connected.load(std::memory_order_relaxed)       ? "advertising"
            : link.authenticated.load(std::memory_order_relaxed) ? "phone connected and authenticated"
                                                                  : "phone connected, not authenticated",
            static_cast<unsigned>(link.mtu.load(std::memory_order_relaxed)),
            static_cast<double>(now.framesNotified - windowStart_.framesNotified) / seconds,
            static_cast<unsigned>(link.writesDropped.load(std::memory_order_relaxed)),
            static_cast<unsigned>(link.notifyFailures.load(std::memory_order_relaxed)));
  }

  const CanBusCounters& bus = sources_.bus.counters();
  const DiagnosticsSummary diagnostics = sources_.diagnostics->summary();
  if (!diagnostics.obd) {
    logInfo(kTag, "obd: off, %s%s", sources_.bus.transmitting() ? "but the bus is active" : "listen-only",
            diagnostics.sweepRunning ? " | service mode: sweep running" : "");
  } else {
    logInfo(kTag,
            "obd: round %" PRIu32 " | %.1f frames/s sent, %" PRIu32 " never acknowledged, %" PRIu32
            " refused, %" PRIu32 " bus-off recoveries | stored codes: %s | poller %s%s",
            diagnostics.rounds, static_cast<double>(now.sent - windowStart_.sent) / seconds,
            bus.sendFailed.load(std::memory_order_relaxed), bus.sendRefused.load(std::memory_order_relaxed),
            bus.recoveries.load(std::memory_order_relaxed), diagnostics.storedCodes.c_str(),
            diagnostics.heldBy.empty() ? "free" : ("held for " + diagnostics.heldBy).c_str(),
            diagnostics.sweepRunning ? " | service mode: sweep running" : "");
  }

  char line[256];
  std::size_t used = 0;
  const std::lock_guard<std::mutex> lock(sources_.storeMutex);
  for (const char* key : kReportedSignals) {
    const auto live = sources_.store.latest(key);
    const int written = live ? std::snprintf(line + used, sizeof line - used, "%s=%g ", key, live->value)
                             : std::snprintf(line + used, sizeof line - used, "%s=- ", key);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof line - used) break;
    used += static_cast<std::size_t>(written);
  }
  logInfo(kTag, "live: %s", line);
}

}  // namespace emcool
