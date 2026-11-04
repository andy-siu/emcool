#pragma once

// Reading one trouble-code list over OBD-II: the request on the functional address 0x7DF,
// ISO-TP reassembly of the reply, the flow control its first frame asks for, and retries
// when a transfer stalls.
//
// Four outcomes, kept apart all the way to the screen: answered; silent (nothing came — NOT
// "no codes"); truncated (a transfer started and stalled, every attempt); not sent (our side,
// not the bike).
//
// Timing: 300 ms for the first reply, 400 ms after each first frame for the rest, and
// up to four retries 120 ms apart, for stalls only — silence and refusals are answers.
// Worst case about 4 s.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "emcool/obd/bus.hpp"
#include "emcool/obd/iso_tp.hpp"
#include "emcool/obd/obd_dtc.hpp"

namespace emcool::obd {

struct DtcReadOutcome {
  enum class Kind : std::uint8_t { kAnswered, kSilent, kTruncated, kNotSent };
  Kind outcome = Kind::kSilent;
  int mode = 0;
  // kAnswered
  ObdDtcResponse response;
  // kTruncated and kNotSent
  std::string reason;
};

// One line for the log: "mode 0x03: 39 stored code(s)", "mode 0x07: NO RESPONSE — …".
std::string describeReadOutcome(const DtcReadOutcome& result);

class TroubleCodeReader {
 public:
  static constexpr std::uint32_t kFunctionalRequestId = 0x7df;
  static constexpr std::int64_t kFirstReplyTimeoutMs = 300;
  static constexpr std::int64_t kTransferTimeoutMs = 400;
  static constexpr int kRetryAttempts = 4;
  static constexpr std::int64_t kRetryGapMs = 120;

  TroubleCodeReader(CanTransmitter& bus, DeadlineOrder& order) : bus_(bus), order_(order) {}

  // requestTroubleCodeList(mode), for 03, 07 or 0A only. The outcome comes from
  // takeOutcome(), at once if the send failed, or later. While another read is running (or
  // for any other mode) it's refused, and the not-sent outcome returned here instead.
  std::optional<DtcReadOutcome> start(int mode, Now now);
  bool busy() const { return phase_ != Phase::kIdle || outcome_.has_value(); }
  // An attempt is on the bus right now.
  bool transferInFlight() const { return phase_ == Phase::kAttempt; }

  // A frame in 0x7E0-0x7EF. True if it was this read's; false leaves it for the PID poller.
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, Now now);
  void tick(Now now);
  std::optional<Deadline> nextDeadline() const;
  std::optional<DtcReadOutcome> takeOutcome();

 private:
  enum class Phase : std::uint8_t { kIdle, kAttempt, kRetryGap };

  void attempt(Now now);
  void expire(Now now);
  // One attempt ended: retry a stall, or hand the outcome over.
  void attemptEnded(DtcReadOutcome result, Now now);
  bool addressesService(std::span<const std::uint8_t> data) const;
  void sendFlowControl(std::uint32_t responseId);

  CanTransmitter& bus_;
  DeadlineOrder& order_;
  Phase phase_ = Phase::kIdle;
  int mode_ = 0;
  int attempt_ = 0;
  std::int64_t attemptStartedMs_ = 0;
  Deadline deadline_ = {0, 0};
  bool sawFirstFrame_ = false;
  std::optional<std::string> failure_;
  IsoTpReassembler reassembler_;
  std::optional<DtcReadOutcome> outcome_;
};

}  // namespace emcool::obd
