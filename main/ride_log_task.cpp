#include "ride_log_task.hpp"

#include "emcool/log.hpp"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace emcool {

namespace {

constexpr const char* kTag = "ride-log";
constexpr std::int64_t kMountRetryMs = 30000;
constexpr std::uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 4;
// Core 0, away from the decode task; the SPI transfers are DMA, so this mostly waits.
constexpr BaseType_t kTaskCore = 0;

std::int64_t monotonicMs() { return esp_timer_get_time() / 1000; }

void releaseAsLost(ride_log::SegmentBuffer& buffer, RideLogTaskCounters& counters) {
  counters.segmentsLost.fetch_add(1, std::memory_order_relaxed);
  counters.readingsLost.fetch_add(static_cast<std::uint32_t>(buffer.rowCount), std::memory_order_relaxed);
  buffer.heldByWriter.store(false, std::memory_order_release);
}

bool tryToOpen(RideLogTaskContext& context) {
  if (!context.card.mount(CONFIG_EMCOOL_SD_SCK_GPIO, CONFIG_EMCOOL_SD_MOSI_GPIO, CONFIG_EMCOOL_SD_MISO_GPIO,
                          CONFIG_EMCOOL_SD_CS_GPIO)) {
    return false;
  }
  context.counters.cardMounted.store(true, std::memory_order_relaxed);
  if (!context.writer.open()) {
    context.card.unmount();
    context.counters.cardMounted.store(false, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void rideLogTask(void* argument) {
  RideLogTaskContext& context = *static_cast<RideLogTaskContext*>(argument);
  std::int64_t nextOpenAttemptMs = 0;
  bool backgroundWorkPending = false;

  while (true) {
    if (!context.writer.isOpen()) {
      // No card: segments keep arriving and are released as lost, so the decode side never
      // runs out of buffers and the loss is counted.
      ride_log::SegmentBuffer* buffer = nullptr;
      if (context.queue.receive(buffer, pdMS_TO_TICKS(1000))) {
        releaseAsLost(*buffer, context.counters);
      }
      if (monotonicMs() >= nextOpenAttemptMs) {
        nextOpenAttemptMs = monotonicMs() + kMountRetryMs;
        backgroundWorkPending = tryToOpen(context);
      }
      continue;
    }

    ride_log::SegmentBuffer* buffer = nullptr;
    if (context.queue.receive(buffer, backgroundWorkPending ? 0 : pdMS_TO_TICKS(1000))) {
      if (context.writer.write(*buffer)) {
        buffer->heldByWriter.store(false, std::memory_order_release);
      } else {
        // The writer has closed itself. The card may have been pulled: unmount, and the
        // retry above opens a fresh boot directory once it is back.
        releaseAsLost(*buffer, context.counters);
        context.card.unmount();
        context.counters.cardMounted.store(false, std::memory_order_relaxed);
        nextOpenAttemptMs = monotonicMs() + kMountRetryMs;
      }
      backgroundWorkPending = true;
      continue;
    }
    backgroundWorkPending = context.writer.prepareAhead();
  }
}

}  // namespace

bool FreeRtosSegmentQueue::create(UBaseType_t depth) {
  queue_ = xQueueCreate(depth, sizeof(ride_log::SegmentBuffer*));
  return queue_ != nullptr;
}

bool FreeRtosSegmentQueue::submit(ride_log::SegmentBuffer& buffer) {
  ride_log::SegmentBuffer* pointer = &buffer;
  return xQueueSend(queue_, &pointer, 0) == pdTRUE;
}

bool FreeRtosSegmentQueue::receive(ride_log::SegmentBuffer*& buffer, TickType_t timeout) {
  return xQueueReceive(queue_, &buffer, timeout) == pdTRUE;
}

ble_server::StorageStatus RideLogStorageReport::storage() {
  ble_server::StorageStatus status = {};
  status.logging = counters_.cardMounted.load(std::memory_order_relaxed);
  status.files = writer_.counters().partsOnCard.load(std::memory_order_relaxed);
  status.bytes = std::uint64_t{status.files} * writer_.partBytes();
  if (const std::optional<CardUsage> usage = status.logging ? cardUsage() : std::nullopt) {
    status.freeBytes = usage->freeBytes;
    status.totalBytes = usage->totalBytes;
  }
  return status;
}

bool startRideLogTask(RideLogTaskContext& context) {
  if (CONFIG_EMCOOL_SD_SCK_GPIO < 0 || CONFIG_EMCOOL_SD_MOSI_GPIO < 0 || CONFIG_EMCOOL_SD_MISO_GPIO < 0 ||
      CONFIG_EMCOOL_SD_CS_GPIO < 0) {
    logWarning(kTag, "microSD pins are set to -1 (idf.py menuconfig → emcool); the ride log is off");
    return false;
  }
  if (xTaskCreatePinnedToCore(rideLogTask, "ride-log", kTaskStackBytes, &context, kTaskPriority, nullptr, kTaskCore) !=
      pdPASS) {
    logError(kTag, "could not create the ride-log task");
    return false;
  }
  return true;
}

}  // namespace emcool
