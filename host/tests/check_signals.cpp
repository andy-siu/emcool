// SignalStore against cool-eva's recordArrival() rules, and the ride-log exclusion list
// against the registry it names keys from.

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/can/frame.hpp"
#include "emcool/signals/ride_log_policy.hpp"
#include "emcool/signals/signal_store.hpp"

namespace {

struct Appended {
  std::string key;
  double value;
  std::int64_t wallClockMs;
};

class CapturingSink : public emcool::signals::RideLogSink {
 public:
  void appendReading(std::int64_t wallClockMs, std::int64_t, const emcool::signals::SignalDef& signal,
                     double value) override {
    appended.push_back({signal.key, value, wallClockMs});
  }
  std::vector<Appended> appended;
};

void checkRegistryIsSortedAndSearchable();
void checkExclusionListNamesRealSignals();
void checkDeadbandGate();
void checkNonFiniteIsIgnored();
void checkExcludedSignalsStayLive();
void checkUnknownKeyIsCountedNotStored();
void checkJsRound();

}  // namespace

int main() {
  checkRegistryIsSortedAndSearchable();
  checkExclusionListNamesRealSignals();
  checkDeadbandGate();
  checkNonFiniteIsIgnored();
  checkExcludedSignalsStayLive();
  checkUnknownKeyIsCountedNotStored();
  checkJsRound();
  std::printf("check_signals: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}

namespace {

void checkRegistryIsSortedAndSearchable() {
  const auto registry = emcool::signals::signalRegistry();
  EMCOOL_CHECK(registry.size() == emcool::signals::kSignalCount, "registry holds %zu", registry.size());
  for (std::size_t index = 1; index < registry.size(); index++) {
    EMCOOL_CHECK(std::strcmp(registry[index - 1].key, registry[index].key) < 0, "%s is not before %s",
                 registry[index - 1].key, registry[index].key);
  }
  for (std::size_t index = 0; index < registry.size(); index++) {
    const auto found = emcool::signals::findSignal(registry[index].key);
    EMCOOL_CHECK(found && *found == index, "findSignal(%s) missed", registry[index].key);
  }
  EMCOOL_CHECK(!emcool::signals::findSignal("not_a_signal"), "found a signal that doesn't exist");
  EMCOOL_CHECK(!emcool::signals::findSignal(""), "found the empty key");
}

// A typo or a cool-eva rename here would silently exclude nothing.
void checkExclusionListNamesRealSignals() {
  for (const std::string_view key : emcool::signals::kExcludedFromRideLog) {
    EMCOOL_CHECK(emcool::signals::findSignal(key).has_value(), "excluded key %.*s is not in the registry",
                 static_cast<int>(key.size()), key.data());
  }
}

// cool-eva logs the first reading, then only a change strictly greater than the deadband.
// The deadband is read from the registry rather than assumed. Starting at 0 makes the
// "exactly one deadband away" step an exact comparison in floating point.
void checkDeadbandGate() {
  constexpr const char* kKey = "pack_kw";
  const auto index = emcool::signals::findSignal(kKey);
  EMCOOL_CHECK(index.has_value(), "%s missing", kKey);
  if (!index) return;
  const double deadband = emcool::signals::signalRegistry()[*index].deadband;
  EMCOOL_CHECK(deadband > 0, "%s has no deadband to test", kKey);

  CapturingSink sink;
  emcool::signals::SignalStore store(sink);
  const double withinDeadband = deadband * 1.5 + deadband * 0.9;
  store.record(kKey, 0.0, 1000, 1);
  store.record(kKey, deadband, 1001, 2);
  store.record(kKey, deadband * 1.5, 1002, 3);
  store.record(kKey, withinDeadband, 1003, 4);
  EMCOOL_CHECK(sink.appended.size() == 2, "appended %zu readings, expected first + one past the deadband",
               sink.appended.size());
  if (sink.appended.size() == 2) {
    EMCOOL_CHECK(sink.appended[0].value == 0.0, "first appended %g", sink.appended[0].value);
    EMCOOL_CHECK(sink.appended[1].value == deadband * 1.5, "second appended %g", sink.appended[1].value);
    EMCOOL_CHECK(sink.appended[1].wallClockMs == 1002, "second stamped %lld",
                 static_cast<long long>(sink.appended[1].wallClockMs));
  }
  // Live state and arrival time follow every reading, logged or not.
  const auto live = store.latest(kKey);
  EMCOOL_CHECK(live && live->value == withinDeadband, "live value didn't follow");
  const auto age = store.ageMs(kKey, 10);
  EMCOOL_CHECK(age && *age == 6, "age is %lld, expected 6", age ? static_cast<long long>(*age) : -1LL);
}

void checkNonFiniteIsIgnored() {
  CapturingSink sink;
  emcool::signals::SignalStore store(sink);
  store.record("soc", std::numeric_limits<double>::quiet_NaN(), 1, 1);
  store.record("soc", std::numeric_limits<double>::infinity(), 2, 2);
  EMCOOL_CHECK(sink.appended.empty(), "a non-finite value was logged");
  EMCOOL_CHECK(!store.latest("soc"), "a non-finite value reached live state");
}

void checkExcludedSignalsStayLive() {
  CapturingSink sink;
  emcool::signals::SignalStore store(sink);
  for (const std::string_view key : emcool::signals::kExcludedFromRideLog) {
    store.record(key, 45.5, 1, 1);
    store.record(key, 47.5, 2, 2);
    EMCOOL_CHECK(store.latest(key).has_value(), "%.*s didn't reach live state", static_cast<int>(key.size()),
                 key.data());
  }
  EMCOOL_CHECK(sink.appended.empty(), "%zu excluded readings reached the ride log", sink.appended.size());
  store.record("gps_speed_kmh", 88, 3, 3);
  EMCOOL_CHECK(sink.appended.size() == 1, "gps_speed_kmh should still be logged");
}

void checkUnknownKeyIsCountedNotStored() {
  CapturingSink sink;
  emcool::signals::SignalStore store(sink);
  store.record("definitely_not_registered", 1, 1, 1);
  EMCOOL_CHECK(store.unknownKeyCount() == 1, "unknown count %u", store.unknownKeyCount());
  EMCOOL_CHECK(sink.appended.empty(), "an unregistered key was logged");
}

void checkJsRound() {
  using emcool::can::jsRound;
  EMCOOL_CHECK(jsRound(2.5) == 3, "2.5 → %g", jsRound(2.5));
  EMCOOL_CHECK(jsRound(-2.5) == -2, "-2.5 → %g (std::round would give -3)", jsRound(-2.5));
  EMCOOL_CHECK(jsRound(-2.6) == -3, "-2.6 → %g", jsRound(-2.6));
  EMCOOL_CHECK(jsRound(0.49999999999999994) == 0, "0.49999999999999994 → %g", jsRound(0.49999999999999994));
  EMCOOL_CHECK(jsRound(-0.5) == 0, "-0.5 → %g", jsRound(-0.5));
  EMCOOL_CHECK(jsRound(4503599627370495.5) == 4503599627370496.0, "2^52 - 0.5 → %.17g",
               jsRound(4503599627370495.5));
}

}  // namespace
