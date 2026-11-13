#pragma once

// Watches whether a charge-current command took: it is fed the bike's charge-request readings and
// the session state, told when a command goes out, and answers the current verdict. One object the
// diagnostics task owns, fed by observe() calls from the decode path; a command settles in a check
// inside observe()/state() against the clock, not on a timer.
//
// It records charge_cmd_a (on send) and, when a command settles, charge_cmd_ack, charge_cmd_ack_seq
// and (for "took") charge_cmd_ack_ms — through the same SignalRecorder the OBD side uses.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "emcool/obd/bus.hpp"  // obd::SignalRecorder
#include "emcool/vcu/charge_acknowledge.hpp"
#include "emcool/vcu/charge_command.hpp"

namespace emcool::vcu {

struct ChargeAckState {
  ChargeAckVerdict verdict;
  std::string message;
};

class ChargeAckWatch {
 public:
  explicit ChargeAckWatch(obd::SignalRecorder& recorder) : recorder_(recorder) {}

  // A command went out. Settles any pending one first (superseded), then arms this one. Records
  // charge_cmd_a. `latestValue` gives the ack signal's current reading for the no-samples fallback.
  void noteSent(ChargeMode mode, double amps, std::int64_t nowMs);
  // A decoded ack-signal reading (fast_dc_target_a / charge_limit_a), or a charge_manager_state
  // change. Mirrors ack-watch's onChange: a non-charging session forgets; an elapsed window settles.
  void observeAckSignal(ChargeMode mode, double amps, std::int64_t nowMs);
  void observeSessionState(double state, std::int64_t nowMs);
  // The current readout, or nullopt when nothing has been commanded this session.
  std::optional<ChargeAckState> state(std::int64_t nowMs) const;

 private:
  struct Pending {
    ChargeMode mode;
    double amps;
    std::int64_t sentAtMs;
  };
  struct Stamped {
    ChargeMode mode;
    AckSample sample;
  };
  std::vector<AckSample> samplesFor(const Pending& command) const;
  void settle(const Pending& command, bool hasSuperseded, std::int64_t supersededAtMs, std::int64_t nowMs);
  void remember(ChargeMode mode, double amps, std::int64_t nowMs);
  void forget();

  obd::SignalRecorder& recorder_;
  std::optional<Pending> pending_;
  std::optional<ChargeAckState> latest_;
  std::vector<Stamped> samples_;
  int settles_ = 0;
  // The last reading of each ack signal, for samplesFor's "no samples yet" fallback.
  std::optional<double> lastDcTarget_;
  std::optional<double> lastAcLimit_;
};

}  // namespace emcool::vcu
