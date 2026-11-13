#include "boot_guard.hpp"

#include <cinttypes>
#include <cstddef>

#include "emcool/log.hpp"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace emcool {

namespace {

constexpr const char* kTag = "boot";
// Marks the count as written by this firmware, not left over in uninitialised RTC memory.
constexpr std::uint32_t kMagic = 0x656d636f;  // "emco"
// Tasks started at boot keep allocating for a while after app_main returns (NimBLE's host syncs,
// the first status report, the first card write), so a crash this soon still counts as the boot's.
constexpr std::uint64_t kSettledUs = 30ull * 1000 * 1000;

RTC_NOINIT_ATTR std::uint32_t crashMagic;
RTC_NOINIT_ATTR std::uint32_t crashCount;

const char* resetName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "the reset pin";
    case ESP_RST_SW:
      return "a restart";
    case ESP_RST_PANIC:
      return "a crash (panic)";
    case ESP_RST_INT_WDT:
      return "the interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "the task watchdog";
    case ESP_RST_WDT:
      return "a watchdog";
    case ESP_RST_DEEPSLEEP:
      return "waking from deep sleep";
    case ESP_RST_BROWNOUT:
      return "a brownout (the supply dipped)";
    default:
      return "an unknown cause";
  }
}

bool isCrash(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// Runs inside malloc's failure path, from any task, so it only prints: no allocation, no locks
// beyond the heap's own size queries, which don't take the allocator's.
void allocationFailed(std::size_t size, std::uint32_t caps, const char* function) {
  esp_rom_printf("E (heap) %s could not get %u bytes (caps 0x%" PRIx32 "): %u free, largest block %u\n", function,
                 static_cast<unsigned>(size), caps, static_cast<unsigned>(heap_caps_get_free_size(caps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
}

}  // namespace

bool beginBoot() {
  const esp_reset_reason_t reason = esp_reset_reason();
  if (crashMagic != kMagic || reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
    crashMagic = kMagic;
    crashCount = 0;
  }
  if (isCrash(reason)) crashCount++;
  heap_caps_register_failed_alloc_callback(allocationFailed);

  if (crashCount == 0) {
    logInfo(kTag, "reset by %s", resetName(reason));
  } else {
    logWarning(kTag, "reset by %s: %" PRIu32 " crash%s in a row before startup finished", resetName(reason), crashCount,
               crashCount == 1 ? "" : "es");
  }
  if (crashCount < kCrashesBeforeSafeMode) return false;
  logError(kTag,
           "SAFE MODE: %" PRIu32 " crashes in a row, so only the serial console runs (no CAN, OBD, ride log, "
           "Bluetooth or Wi-Fi). Undo the last setting you changed (`show`, then `unset NAME` or `set NAME 0`), then "
           "`restart`. A power cycle also leaves safe mode.",
           crashCount);
  // A restart from the console then boots normally; a crash then counts from here.
  crashCount = 0;
  return true;
}

void bootFinished() {
  logInfo(kTag, "startup finished");
  noteHeap("startup");
  const esp_timer_create_args_t settled = {
      .callback = [](void*) {
        crashCount = 0;
        noteHeap("the first 30 s");
      },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "boot-settled",
      .skip_unhandled_events = true,
  };
  esp_timer_handle_t timer = nullptr;
  const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time());
  if (esp_timer_create(&settled, &timer) != ESP_OK ||
      esp_timer_start_once(timer, now < kSettledUs ? kSettledUs - now : 1) != ESP_OK) {
    crashCount = 0;  // No timer: count only crashes before this point.
  }
}

void noteHeap(const char* step) {
  logInfo(kTag, "heap after %s: %u free, largest block %u, lowest so far %u", step,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
}

}  // namespace emcool
