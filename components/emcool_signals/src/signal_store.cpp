#include "emcool/signals/signal_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "emcool/log.hpp"
#include "emcool/signals/ride_log_policy.hpp"

namespace emcool::signals {

std::optional<std::size_t> findSignal(std::string_view key) {
  const std::span<const SignalDef> registry = signalRegistry();
  const auto found = std::lower_bound(registry.begin(), registry.end(), key,
                                      [](const SignalDef& signal, std::string_view wanted) {
                                        return std::string_view(signal.key) < wanted;
                                      });
  if (found == registry.end() || std::string_view(found->key) != key) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - registry.begin());
}

bool isExcludedFromRideLog(std::string_view key) {
  return std::find(std::begin(kExcludedFromRideLog), std::end(kExcludedFromRideLog), key) !=
         std::end(kExcludedFromRideLog);
}

SignalStore::SignalStore(RideLogSink& rideLog) : rideLog_(rideLog) {
  const std::span<const SignalDef> registry = signalRegistry();
  for (std::size_t index = 0; index < registry.size(); index++) {
    states_[index].excludedFromRideLog = isExcludedFromRideLog(registry[index].key);
  }
}

void SignalStore::record(std::string_view key, double value, std::int64_t wallClockMs, std::int64_t monotonicMs) {
  if (!std::isfinite(value)) return;
  const std::optional<std::size_t> index = findSignal(key);
  if (!index) {
    noteUnknownKey(key);
    return;
  }
  SignalState& state = states_[*index];
  state.liveValue = value;
  state.liveWallClockMs = wallClockMs;
  state.lastSeenMonotonicMs = monotonicMs;
  state.seen = true;

  // Arrival is refreshed above, outside the deadband, on purpose: a button nobody presses
  // still repeats on the bus, and skipping repeats would make it look like it went missing.
  const SignalDef& signal = signalRegistry()[*index];
  if (state.everLogged && !(std::fabs(value - state.lastLogged) > signal.deadband)) {
    return;
  }
  state.lastLogged = value;
  state.everLogged = true;
  changed_.set(*index);
  if (!state.excludedFromRideLog) {
    rideLog_.appendReading(wallClockMs, monotonicMs, signal, value);
  }
}

std::optional<LiveValue> SignalStore::latest(std::string_view key) const {
  const std::optional<std::size_t> index = findSignal(key);
  if (!index || !states_[*index].seen) {
    return std::nullopt;
  }
  return LiveValue{states_[*index].liveValue, states_[*index].liveWallClockMs};
}

std::optional<std::int64_t> SignalStore::ageMs(std::string_view key, std::int64_t nowMonotonicMs) const {
  const std::optional<std::size_t> index = findSignal(key);
  if (!index || !states_[*index].seen) {
    return std::nullopt;
  }
  return nowMonotonicMs - states_[*index].lastSeenMonotonicMs;
}

// An unregistered key has no slot: state is a fixed array sized by the registry, so it
// can't be kept — the host checks make sure every decoded key is registered, which
// makes this a can't-happen that must be loud when it does.
void SignalStore::noteUnknownKey(std::string_view key) {
  unknownKeyCount_++;
  if (unknownKeyCount_ < nextUnknownKeyReportAt_) {
    return;
  }
  nextUnknownKeyReportAt_ *= 2;
  logError("signals", "dropped a reading for unregistered key \"%.*s\" (%u such readings so far)",
           static_cast<int>(key.size()), key.data(), static_cast<unsigned>(unknownKeyCount_));
}

}  // namespace emcool::signals
