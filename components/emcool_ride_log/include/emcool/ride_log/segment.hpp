#pragma once

// One segment on its way to the card: filled by the decode task, then handed to the writer
// task, which finalizes the record header and writes it. `heldByWriter` is the hand-off: the
// decode side never touches a buffer while it is set.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "emcool/clock/system_clock_sync.hpp"
#include "emcool/ride_log/record.hpp"

namespace emcool::ride_log {

struct SegmentBuffer {
  alignas(4) std::uint8_t bytes[kRecordCapacityBytes] = {};
  std::size_t payloadLength = 0;
  std::size_t rowCount = 0;
  clock::ClockTrust trust = clock::ClockTrust::kNeverSynced;
  std::int64_t openedAtMonotonicMs = 0;
  std::atomic<bool> heldByWriter{false};

  char* payload() { return reinterpret_cast<char*>(bytes + kRecordHeaderBytes); }
};

// The writer side of the hand-off. submit() either takes the buffer (and clears
// heldByWriter when it is done with it) or returns false and leaves it with the caller.
class SegmentQueue {
 public:
  virtual ~SegmentQueue() = default;
  virtual bool submit(SegmentBuffer& buffer) = 0;
};

}  // namespace emcool::ride_log
