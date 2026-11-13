#pragma once

// Two things that keep a setting that costs too much RAM from bricking the bike:
//
//   - noteHeap(): one log line per boot step with the heap left, so a boot log shows where the
//     memory went. And a hook on every failed allocation, which logs its size and what was left,
//     so the line before a panic says what ran out.
//   - safe mode: a count, kept in RTC memory across resets, of boots that crashed (panic or
//     watchdog) in their first 30 s. After kCrashesBeforeSafeMode in a row, the
//     next boot skips everything that takes RAM (CAN, OBD, the ride log, Bluetooth, Wi-Fi) and
//     starts only the serial console, so the setting can be undone. Power-on clears the count.

#include <cstdint>

namespace emcool {

inline constexpr std::uint32_t kCrashesBeforeSafeMode = 3;

// Call first in app_main. Logs why the board reset, counts a crash, and installs the failed-
// allocation hook. Returns true when this boot should run in safe mode.
bool beginBoot();
// Call once startup is complete. If the board is still up 30 s after boot, the count starts again.
void bootFinished();

// "heap after <step>: N free, largest block M, lowest so far L".
void noteHeap(const char* step);

}  // namespace emcool
