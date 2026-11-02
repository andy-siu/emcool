#pragma once

// The one-shot keys-paired read from the E-LOCK ECU, ported from cool-eva src/can/elock.ts:
// KWP2000 ReadDataByLocalIdentifier `21 99` on 0x791, answered on 0x790. If the default
// session won't answer, it opens a standard session (`10 81`), asks again, and closes it
// (`20`). The answer becomes the keys_paired signal; no answer leaves it unset rather than 0.
// Each request waits up to 300 ms.

#include <cstdint>
#include <optional>
#include <span>

#include "emcool/obd/bus.hpp"

namespace emcool::obd {

class ElockReader {
 public:
  static constexpr std::uint32_t kRequestId = 0x791;
  static constexpr std::uint32_t kResponseId = 0x790;
  static constexpr std::int64_t kResponseTimeoutMs = 300;

  ElockReader(CanTransmitter& bus, SignalRecorder& recorder, DeadlineOrder& order)
      : bus_(bus), recorder_(recorder), order_(order) {}

  // readKeysPairedOnce()
  void start(Now now);
  static bool isResponse(std::uint32_t id) { return id == kResponseId; }
  void handleResponse(std::span<const std::uint8_t> data, Now now);
  void tick(Now now);
  std::optional<Deadline> nextDeadline() const;
  bool done() const { return step_ == Step::kDone; }

 private:
  enum class Step : std::uint8_t { kIdle, kReadKeys, kStartSession, kReadKeysInSession, kStopSession, kDone };

  // Sends the step's request; a send that fails is a missing answer, at once.
  void send(Step step, Now now);
  // The step's answer, or nullopt for none.
  void answered(std::optional<std::span<const std::uint8_t>> response, Now now);
  void finish(std::optional<int> keysPaired);

  CanTransmitter& bus_;
  SignalRecorder& recorder_;
  DeadlineOrder& order_;
  Step step_ = Step::kIdle;
  bool waiting_ = false;
  Deadline deadline_ = {0, 0};
  std::optional<int> keysPaired_;
};

}  // namespace emcool::obd
