#pragma once

// The live telemetry the phone dashboard subscribes to, as cool-eva src/ws.ts DashboardMessage
// JSON: {"type":"snapshot"|"patch","ts":…,"signals":{"key":{"value","unit","group","ts"}}}.
// cool-eva sends a whole snapshot as one message; here one is ~25 KB of JSON against ~4.5 KB
// messages, so it goes in chunks. That is safe because the dashboard's store (cool-eva
// public/lib/store.js apply()) folds every message in key by key, snapshot or patch alike.
// The one thing a chunk boundary can cost: the pack-resistance estimate pairs pack_v with
// pack_a only within one message, so a snapshot that splits them gives it no pair — patches,
// which carry each 0x200 frame's values together, still do.

#include <cstddef>
#include <cstdint>
#include <span>

#include "emcool/signals/signal_store.hpp"

namespace emcool::ble_server {

class TelemetryStream {
 public:
  void beginSnapshot() {
    snapshotCursor_ = 0;
    snapshotting_ = true;
  }
  void cancelSnapshot() { snapshotting_ = false; }
  bool snapshotInProgress() const { return snapshotting_; }

  // The next chunk of the snapshot as a STREAM header, or 0 when it is complete.
  std::size_t nextSnapshotChunk(const signals::SignalStore& store, std::int64_t wallClockMs, std::span<char> out);

  // A patch of what changed past its deadband since the last patch, taking those signals
  // from the store as they fit; 0 when nothing has changed. Call again if anyChanged().
  std::size_t nextPatch(signals::SignalStore& store, std::int64_t wallClockMs, std::span<char> out);

 private:
  std::size_t snapshotCursor_ = 0;
  bool snapshotting_ = false;
};

}  // namespace emcool::ble_server
