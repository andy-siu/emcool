#pragma once

// The live value of every signal, and the log-on-change gate in front of the ride log.
// Every reading refreshes live state and its arrival time; only a change past the signal's
// deadband is passed to the ride log and marked changed for the phone's live stream. Not
// thread-safe: the firmware guards it with one mutex shared by the decode task and the
// Bluetooth task.

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string_view>

#include "emcool/signals/registry.hpp"

namespace emcool::signals {

class RideLogSink {
 public:
  virtual ~RideLogSink() = default;
  // `monotonicMs` is when the reading arrived, for timing segments; `wallClockMs` stamps it.
  virtual void appendReading(std::int64_t wallClockMs, std::int64_t monotonicMs, const SignalDef& signal,
                             double value) = 0;
};

struct LiveValue {
  double value;
  std::int64_t wallClockMs;
};

class SignalStore {
 public:
  explicit SignalStore(RideLogSink& rideLog);
  SignalStore(const SignalStore&) = delete;
  SignalStore& operator=(const SignalStore&) = delete;

  // `monotonicMs` times arrivals; `wallClockMs` stamps readings. They are separate because
  // the wall clock gets stepped when GPS time arrives, and an age must not jump with it.
  void record(std::string_view key, double value, std::int64_t wallClockMs, std::int64_t monotonicMs);

  std::optional<LiveValue> latest(std::string_view key) const;
  std::optional<std::int64_t> ageMs(std::string_view key, std::int64_t nowMonotonicMs) const;

  std::uint32_t unknownKeyCount() const { return unknownKeyCount_; }

  // By registry index, for building snapshots and status without a lookup per signal.
  bool seenAt(std::size_t index) const { return states_[index].seen; }
  LiveValue liveAt(std::size_t index) const { return {states_[index].liveValue, states_[index].liveWallClockMs}; }
  std::int64_t lastSeenMonotonicMsAt(std::size_t index) const { return states_[index].lastSeenMonotonicMs; }

  // Signals that passed their deadband since they were last taken: the live stream's patches.
  bool changedAt(std::size_t index) const { return changed_.test(index); }
  void clearChanged(std::size_t index) { changed_.reset(index); }
  void clearAllChanged() { changed_.reset(); }
  bool anyChanged() const { return changed_.any(); }

 private:
  struct SignalState {
    double liveValue = 0;
    std::int64_t liveWallClockMs = 0;
    std::int64_t lastSeenMonotonicMs = 0;
    double lastLogged = 0;
    bool seen = false;
    bool everLogged = false;
    bool excludedFromRideLog = false;
  };

  void noteUnknownKey(std::string_view key);

  RideLogSink& rideLog_;
  std::array<SignalState, kSignalCount> states_{};
  std::bitset<kSignalCount> changed_;
  std::uint32_t unknownKeyCount_ = 0;
  std::uint32_t nextUnknownKeyReportAt_ = 1;
};

}  // namespace emcool::signals
