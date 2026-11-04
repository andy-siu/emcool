#pragma once

// The automatic charge-current controller: the sample buffers, the 60 s tick, the rider-override
// standdown, the mode and reason, and the records. Its decision is charge_auto_curve.hpp's pure
// decideChargeCurrent; this is the stateful shell that feeds it live signals and acts on it.
//
// emcool feeds the three signals it watches (batt_temp_hi, soc, dc_charge_limit_selected_a) and
// charge_manager_state in through observe*() from the decode path, exactly as the ack-watch is
// fed. The tick runs on the diagnostics loop. It commands through a sink, which the diagnostics
// task backs with the write runner's charge-current (origin=automatic).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "emcool/async/loop.hpp"
#include "emcool/async/task.hpp"
#include "emcool/obd/bus.hpp"  // obd::SignalRecorder
#include "emcool/vcu/charge_auto_curve.hpp"

namespace emcool::vcu {

inline constexpr std::int64_t kAutoTickMs = 60000;
enum class ChargeAutoMode : std::uint8_t { kOff, kAutomatic };
inline int chargeAutoModeCode(ChargeAutoMode mode) { return mode == ChargeAutoMode::kAutomatic ? 1 : 0; }

struct ChargeAutoState {
  ChargeAutoMode mode = ChargeAutoMode::kOff;
  ChargeAutoReason reason = ChargeAutoReason::kDisabled;
  std::optional<int> commandedAmps;
};

// What the controller commands through. Resolves with whether the command took and a message.
struct ChargeCommandResult {
  bool succeeded;
  std::string message;
};
using ChargeCommandSink = std::function<async::Task<ChargeCommandResult>(int amps)>;

// What the controller reads from live state (latestValue/ageMs), and the monotonic now.
struct ChargeAutoReaders {
  std::function<std::optional<double>(std::string_view key)> latestValue;
  std::function<std::optional<double>(std::string_view key)> ageMs;
  std::function<std::int64_t()> nowMs;
};

class ChargeAutomatic {
 public:
  // `enabled` is CHARGE_AUTO_ENABLED. When false the controller is inert: mode off, reason disabled,
  // setMode ignored (with a warning), and no tick is scheduled.
  ChargeAutomatic(bool enabled, ChargeCommandSink sink, ChargeAutoReaders readers, obd::SignalRecorder& recorder);

  // Starts the 60 s tick (and publishes the initial mode/reason). Call once, on the owning loop.
  void start();
  void stop();

  ChargeAutoState state() const;
  void setMode(ChargeAutoMode mode);
  // Told BEFORE a charge current reaches the bus (from the write runner's onChargeCurrentOutgoing).
  // A manual one stands the controller down for this charge.
  void noteChargeCurrentOutgoing(int amps, bool manual);

  // The watched signals, as they arrive. Called from the decode path on the owning loop.
  void observeTemperature(double celsius);
  void observeSoc(double percent);
  void observeSelectedCurrent(double amps);        // dc_charge_limit_selected_a — a rider setpoint?
  void observeSessionState(double state);          // charge_manager_state — a new session forgets

  // The tick, exposed for the check; start() arms it on the loop.
  async::Task<> runTick();

 private:
  ChargeAutoDecision decide() const;
  void rememberTemperature(double celsius);
  void rememberSoc(double percent);
  void forgetSession();
  bool isRiderSetpoint(double observedAmps) const;
  void publishMode();
  void standDown();
  void setReason(ChargeAutoReason reason);

  bool enabled_;
  ChargeCommandSink sink_;
  ChargeAutoReaders readers_;
  obd::SignalRecorder& recorder_;

  ChargeAutoMode mode_ = ChargeAutoMode::kAutomatic;
  ChargeAutoReason reason_ = ChargeAutoReason::kNoHistory;
  std::optional<std::pair<int, std::int64_t>> lastCommand_;  // amps, atMs
  std::optional<int> lastSentAmps_;
  bool riderOverride_ = false;
  std::vector<TemperatureSample> samples_;
  std::vector<SocSample> socSamples_;
  bool firstSocMayNotBeACrossing_ = false;
  bool inFlight_ = false;
  std::optional<double> lastSessionState_;
  async::TimerId timer_;
  bool started_ = false;
};

}  // namespace emcool::vcu
