#pragma once

// The OBD-II poller: mode 01 PIDs (speed, rpm, the three temperatures, 12 V, SOH, load,
// distance) one at a time on the functional address 0x7DF, each waited for up to 200 ms, a
// round every 500 ms. Every 20th round adds the diagnostic PIDs (MIL, the freeze-frame code,
// time and distance since clearing); every 120th — about a minute — reads the stored trouble
// codes, and every tenth of those the pending and permanent ones too.
//
// It parks for a hold (obd_hold.hpp) at each check: before every PID, before each list read,
// and at the start of each round.

#include <cstdint>
#include <optional>
#include <span>

#include "emcool/obd/bus.hpp"
#include "emcool/obd/obd_hold.hpp"
#include "emcool/obd/stored_codes.hpp"
#include "emcool/obd/transmit_log.hpp"
#include "emcool/obd/trouble_code_reader.hpp"

namespace emcool::obd {

class ObdPoller {
 public:
  static constexpr std::uint32_t kRequestId = 0x7df;
  static constexpr std::uint32_t kResponseLowId = 0x7e0;
  static constexpr std::uint32_t kResponseHighId = 0x7ef;
  static constexpr std::int64_t kPidTimeoutMs = 200;
  static constexpr std::int64_t kHoldPollMs = 100;
  static constexpr std::int64_t kIntervalMs = 500;
  static constexpr int kDiagnosticRoundDivisor = 20;
  static constexpr int kStoredDtcRoundDivisor = 120;
  static constexpr int kSilentModeReadEvery = 10;

  ObdPoller(CanTransmitter& bus, SignalRecorder& recorder, TroubleCodeReader& codes, StoredCodes& stored,
            ObdHold& hold, DeadlineOrder& order, TransmitFailureLog& transmitLog,
            std::int64_t intervalMs = kIntervalMs)
      : bus_(bus),
        recorder_(recorder),
        codes_(codes),
        stored_(stored),
        hold_(hold),
        order_(order),
        transmitLog_(transmitLog),
        intervalMs_(intervalMs) {}

  // startObdPoller(): the first round begins now.
  void start(Now now);
  void stop() { phase_ = Phase::kStopped; }
  bool running() const { return phase_ != Phase::kStopped; }

  static bool isResponse(std::uint32_t id) { return id >= kResponseLowId && id <= kResponseHighId; }
  // Any frame in 0x7E0-0x7EF. A trouble-code read in progress gets
  // first look; a mode 01 answer to the PID being waited for is filed.
  void handleResponse(std::uint32_t id, std::span<const std::uint8_t> data, Now now);
  void tick(Now now);
  // A trouble-code read it started has finished: carry on at once, as an await would.
  void takeReadOutcome(Now now);
  std::optional<Deadline> nextDeadline() const;

  std::uint32_t rounds() const { return pollRound_; }

 private:
  enum class Phase : std::uint8_t { kStopped, kSleeping, kAwaitingPid, kReadingCodes };
  enum class CodeStep : std::uint8_t { kStored, kPending, kPermanent };

  void beginRound(Now now);
  // Asks for the next PID due this round, or moves on to the trouble codes.
  void continuePids(Now now);
  void startCodeLists(Now now);
  void startCodeRead(CodeStep step, Now now);
  void codeReadDone(DtcReadOutcome result, Now now);
  void endRound(Now now);
  void sleepFor(std::int64_t delayMs, Now now);
  void fileResponse(std::size_t pidIndex, std::span<const std::uint8_t> response);

  CanTransmitter& bus_;
  SignalRecorder& recorder_;
  TroubleCodeReader& codes_;
  StoredCodes& stored_;
  ObdHold& hold_;
  DeadlineOrder& order_;
  TransmitFailureLog& transmitLog_;
  const std::int64_t intervalMs_;

  Phase phase_ = Phase::kStopped;
  Deadline wakeAt_ = {0, 0};
  std::int64_t roundStartedMs_ = 0;
  std::uint32_t pollRound_ = 0;
  std::uint32_t storedDtcReads_ = 0;
  std::size_t nextPid_ = 0;
  std::size_t awaitingPid_ = 0;
  Deadline pidDeadline_ = {0, 0};
  CodeStep codeStep_ = CodeStep::kStored;
};

}  // namespace emcool::obd
