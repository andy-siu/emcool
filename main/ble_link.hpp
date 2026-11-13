#pragma once

// The phone link on the radio: NimBLE as a peripheral advertising the dashboard's GATT service
// (rx write, tx notify, info read), and a task that runs BleServer. NimBLE's callbacks run
// in its host task and never touch the server: writes and link events are queued to this
// task, and the info characteristic reads a copy this task keeps current. So no lock is
// ever held across a call into NimBLE.

#include <atomic>
#include <cstdint>
#include <mutex>

#include "emcool/ble_server/server.hpp"

namespace emcool {

struct BleLinkCounters {
  std::atomic<bool> connected{false};
  std::atomic<bool> authenticated{false};
  std::atomic<std::uint64_t> framesSent{0};
  std::atomic<std::uint16_t> mtu{23};
  std::atomic<std::uint32_t> writesDropped{0};
  std::atomic<std::uint32_t> notifyFailures{0};
};

// Starts NimBLE and the link task. The server is used from that task only.
bool startBleLink(ble_server::BleServer& server, const char* deviceName, BleLinkCounters& counters);

}  // namespace emcool
