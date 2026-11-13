#include "emcool/obd/obd_poller.hpp"

#include <algorithm>
#include <array>

#include "emcool/log.hpp"

namespace emcool::obd {

namespace {

struct PidDef {
  std::uint8_t pid;
  // 0 for every round.
  int everyNthRound;
};

constexpr int kDiagnostic = ObdPoller::kDiagnosticRoundDivisor;

// The PIDs, in the order they are polled.
constexpr std::array<PidDef, 15> kPids = {{
    {0x0d, 0},           // speed_kmh
    {0x0c, 0},           // motor_rpm
    {0x05, 0},           // bike_coolant_temp
    {0x5c, 0},           // oil_temp
    {0x46, 0},           // ambient_temp
    {0x42, 0},           // aux_12v
    {0x5b, 0},           // soh_pid
    {0x04, 0},           // motor_load_pct
    {0x31, 0},           // dist_since_clear_km
    {0x01, kDiagnostic}, // mil_on, dtc_count
    {0x02, kDiagnostic}, // freeze_frame_dtc
    {0x4e, kDiagnostic}, // time_since_clear_min
    {0x21, kDiagnostic}, // dist_with_mil_km
    {0x4d, kDiagnostic}, // time_with_mil_min
    {0x30, kDiagnostic}, // warmups_since_clear
}};

}  // namespace

void ObdPoller::start(Now now) { beginRound(now); }

void ObdPoller::beginRound(Now now) {
  roundStartedMs_ = now.monotonicMs;
  if (hold_.parkedForHold(now.monotonicMs)) {
    sleepFor(kHoldPollMs, now);
    return;
  }
  pollRound_ += 1;
  nextPid_ = 0;
  continuePids(now);
}

void ObdPoller::continuePids(Now now) {
  while (nextPid_ < kPids.size()) {
    const std::size_t index = nextPid_++;
    const PidDef& def = kPids[index];
    if (def.everyNthRound != 0 && pollRound_ % static_cast<std::uint32_t>(def.everyNthRound) != 0) continue;
    if (hold_.parkedForHold(now.monotonicMs)) {
      endRound(now);
      return;
    }
    const Payload request = {0x02, 0x01, def.pid, 0x55, 0x55, 0x55, 0x55, 0x55};
    if (const std::optional<std::string> problem = bus_.transmit(kRequestId, request)) {
      // The reference's requestPid rejects and ends pollOnce, so a refused PID skips the rest of the
      // round, trouble-code reads included, and the round keeps its pacing (docs/obd.md).
      transmitLog_.log(now.monotonicMs, "poll round failed — skipping the remaining requests", *problem);
      endRound(now);
      return;
    }
    awaitingPid_ = index;
    pidDeadline_ = order_.at(now.monotonicMs + kPidTimeoutMs);
    phase_ = Phase::kAwaitingPid;
    return;
  }
  startCodeLists(now);
}

void ObdPoller::startCodeLists(Now now) {
  if (pollRound_ % kStoredDtcRoundDivisor != 1 || hold_.parkedForHold(now.monotonicMs)) {
    endRound(now);
    return;
  }
  startCodeRead(CodeStep::kStored, now);
}

void ObdPoller::startCodeRead(CodeStep step, Now now) {
  codeStep_ = step;
  const int mode = step == CodeStep::kStored    ? kModeStoredDtcs
                   : step == CodeStep::kPending ? kModePendingDtcs
                                                : kModePermanentDtcs;
  phase_ = Phase::kReadingCodes;
  if (std::optional<DtcReadOutcome> refused = codes_.start(mode, now)) {
    codeReadDone(std::move(*refused), now);
    return;
  }
  // A send that failed has its outcome already.
  takeReadOutcome(now);
}

void ObdPoller::takeReadOutcome(Now now) {
  if (phase_ != Phase::kReadingCodes) return;
  if (std::optional<DtcReadOutcome> outcome = codes_.takeOutcome()) codeReadDone(std::move(*outcome), now);
}

void ObdPoller::codeReadDone(DtcReadOutcome result, Now now) {
  const DtcList list = codeStep_ == CodeStep::kStored    ? DtcList::kStored
                       : codeStep_ == CodeStep::kPending ? DtcList::kPending
                                                         : DtcList::kPermanent;
  stored_.recordRead(result, list, now, recorder_);
  switch (codeStep_) {
    case CodeStep::kStored:
      storedDtcReads_ += 1;
      // Pending and permanent have never answered on this bike, so they're asked rarely.
      if (storedDtcReads_ % kSilentModeReadEvery != 1 || hold_.parkedForHold(now.monotonicMs)) {
        endRound(now);
        return;
      }
      startCodeRead(CodeStep::kPending, now);
      return;
    case CodeStep::kPending:
      if (hold_.parkedForHold(now.monotonicMs)) {
        endRound(now);
        return;
      }
      startCodeRead(CodeStep::kPermanent, now);
      return;
    case CodeStep::kPermanent:
      endRound(now);
      return;
  }
}

void ObdPoller::endRound(Now now) { sleepFor(std::max<std::int64_t>(0, intervalMs_ - (now.monotonicMs - roundStartedMs_)), now); }

// A round that overran its interval still leaves a 1 ms gap, as Node's setTimeout never runs
// sooner than 1 ms.
void ObdPoller::sleepFor(std::int64_t delayMs, Now now) {
  wakeAt_ = order_.at(now.monotonicMs + std::max<std::int64_t>(1, delayMs));
  phase_ = Phase::kSleeping;
}

void ObdPoller::handleResponse(std::uint32_t id, std::span<const std::uint8_t> data, Now now) {
  if (codes_.handleFrame(id, data, now)) {
    takeReadOutcome(now);
    return;
  }
  if (data.size() < 3 || (data[0] >> 4) != 0x0 || data[1] != 0x41) return;
  if (phase_ != Phase::kAwaitingPid || kPids[awaitingPid_].pid != data[2]) return;
  fileResponse(awaitingPid_, data);
  continuePids(now);
}

void ObdPoller::tick(Now now) {
  switch (phase_) {
    case Phase::kStopped:
      return;
    case Phase::kSleeping:
      if (now.monotonicMs >= wakeAt_.atMs) beginRound(now);
      return;
    case Phase::kAwaitingPid:
      // No answer: on to the next PID.
      if (now.monotonicMs >= pidDeadline_.atMs) continuePids(now);
      return;
    case Phase::kReadingCodes:
      takeReadOutcome(now);
      return;
  }
}

std::optional<Deadline> ObdPoller::nextDeadline() const {
  switch (phase_) {
    case Phase::kSleeping:
      return wakeAt_;
    case Phase::kAwaitingPid:
      return pidDeadline_;
    case Phase::kStopped:
    case Phase::kReadingCodes:
      return std::nullopt;
  }
  return std::nullopt;
}

void ObdPoller::fileResponse(std::size_t pidIndex, std::span<const std::uint8_t> response) {
  const double a = response.size() > 3 ? response[3] : 0;
  const double b = response.size() > 4 ? response[4] : 0;
  switch (kPids[pidIndex].pid) {
    case 0x0d:
      recorder_.record("speed_kmh", a);
      return;
    case 0x0c:
      recorder_.record("motor_rpm", (256 * a + b) / 4);
      return;
    case 0x05:
      recorder_.record("bike_coolant_temp", a - 40);
      return;
    case 0x5c:
      recorder_.record("oil_temp", a - 40);
      return;
    case 0x46:
      recorder_.record("ambient_temp", a - 40);
      return;
    case 0x42:
      recorder_.record("aux_12v", (256 * a + b) / 1000);
      return;
    case 0x5b:
      recorder_.record("soh_pid", (a * 100) / 255);
      return;
    case 0x04:
      recorder_.record("motor_load_pct", (a * 100) / 255);
      return;
    case 0x31:
      recorder_.record("dist_since_clear_km", 256 * a + b);
      return;
    case 0x01: {
      const int byte = static_cast<int>(a);
      recorder_.record("mil_on", (byte & 0x80) != 0 ? 1 : 0);
      recorder_.record("dtc_count", byte & 0x7f);
      return;
    }
    case 0x02: {
      const double raw = 256 * a + b;
      recorder_.record(kFreezeFrameDtcKey, raw);
      stored_.recordFreezeFrameDtc(static_cast<std::uint16_t>(raw));
      return;
    }
    case 0x4e:
      recorder_.record("time_since_clear_min", 256 * a + b);
      return;
    case 0x21:
      recorder_.record("dist_with_mil_km", 256 * a + b);
      return;
    case 0x4d:
      recorder_.record("time_with_mil_min", 256 * a + b);
      return;
    case 0x30:
      recorder_.record("warmups_since_clear", a);
      return;
    default:
      return;
  }
}

}  // namespace emcool::obd
