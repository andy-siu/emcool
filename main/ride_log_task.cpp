#include "ride_log_task.hpp"

#include "emcool/log.hpp"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace emcool {

namespace {

constexpr const char* kTag = "ride-log";
constexpr std::int64_t kMountRetryMs = 30000;
constexpr std::int64_t kSlowStepMs = 2000;
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

// Mounts the card if it isn't, then opens the ride log on it. A ride log that won't open (a full
// card, a folder it can't make) leaves the card mounted: the consoles' card commands and the
// download still work on it, and the next attempt tries the ride log again. Only a card that has
// stopped answering is unmounted, so a new one can be mounted in its place.
bool tryToOpen(RideLogTaskContext& context) {
  if (context.card.mounted() && !context.card.responds()) {
    logWarning(kTag, "the card stopped answering; unmounting it");
    context.card.unmount();
    context.counters.cardMounted.store(false, std::memory_order_relaxed);
  }
  if (!context.card.mounted()) {
    if (!context.card.mount()) {
      return false;
    }
    context.counters.cardMounted.store(true, std::memory_order_relaxed);
  }
  context.counters.rideLogOpening.store(true, std::memory_order_relaxed);
  const bool opened = context.writer.open();
  context.counters.rideLogOpening.store(false, std::memory_order_relaxed);
  if (!opened) {
    logWarning(kTag, "the card stays mounted for the card commands; the ride log tries again in %lld s",
               static_cast<long long>(kMountRetryMs / 1000));
  }
  return opened;
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
        // The writer has closed itself. The retry above opens a fresh boot directory, and
        // first unmounts the card if it was pulled, so one put back is mounted again.
        releaseAsLost(*buffer, context.counters);
        nextOpenAttemptMs = monotonicMs() + kMountRetryMs;
      }
      backgroundWorkPending = true;
      continue;
    }
    const std::int64_t stepStartMs = monotonicMs();
    backgroundWorkPending = context.writer.prepareAhead();
    // A step is normally 0.3-0.5 s. ESP-IDF's SD-over-SPI driver spin-waits while the card is busy
    // and never yields, so a card that stalls for seconds keeps core 0 to itself, and past 5 s the
    // task watchdog warns about IDLE0. This line says it was the card.
    const std::int64_t stepMs = monotonicMs() - stepStartMs;
    if (stepMs > kSlowStepMs) {
      logWarning(kTag, "the card took %lld ms over a 64 KB zero-fill step (normally under 0.6 s); a slow or worn card",
                 static_cast<long long>(stepMs));
    }
    // A zero-fill step is 64 KB of card writes, and the next starts at once while there's more to
    // fill. Without a pause, a slow card keeps this task (priority 4, core 0) running long enough
    // to starve everything below it, the idle task included, and the task watchdog fires. One tick
    // costs the fill nothing that matters, and segments still wake it straight away.
    if (backgroundWorkPending) vTaskDelay(1);
  }
}

}  // namespace

bool CardAccess::mount() {
  if (!card_.mount(CONFIG_EMCOOL_SD_SCK_GPIO, CONFIG_EMCOOL_SD_MOSI_GPIO, CONFIG_EMCOOL_SD_MISO_GPIO,
                   CONFIG_EMCOOL_SD_CS_GPIO)) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  mounted_ = true;
  return true;
}

bool CardAccess::responds() {
  // Under the lock, so the status request can't land in the middle of a console's or the
  // download's file operation.
  const std::lock_guard<std::mutex> lock(mutex_);
  return card_.responds();
}

void CardAccess::unmount() {
  const std::lock_guard<std::mutex> lock(mutex_);
  mounted_ = false;
  generation_++;
  card_.unmount();
}

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
