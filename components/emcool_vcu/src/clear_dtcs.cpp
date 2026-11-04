#include <array>
#include <memory>

#include "emcool/log.hpp"
#include "emcool/obd/bus.hpp"
#include "emcool/obd/obd_dtc.hpp"
#include "emcool/obd/obd_poller.hpp"
#include "emcool/obd/trouble_code_reader.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/write_runner.hpp"
#include "emcool/vcu/write_session.hpp"

// OBD Mode 04, and the read-back that is the only thing separating "the bike said 44" from "the
// bike erased its fault memory". The poller is parked first (nothing is sent if it will not
// park) and the gate is re-checked twice inside the hold, because the watchdog cannot cover a
// pre-frame window with nothing in flight to abort.

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr const char* kWhat = "clearing the stored trouble codes";

struct CounterReading {
  std::optional<int> storedCount;
  std::optional<int> distSinceClearKm;
};

std::string describeCounters(const CounterReading& reading) {
  const std::string stored =
      reading.storedCount ? std::to_string(*reading.storedCount) + " stored" : "stored count unread";
  const std::string distance = reading.distSinceClearKm ? std::to_string(*reading.distSinceClearKm) + " km since clear"
                                                        : "distance unread";
  return stored + ", " + distance;
}

std::string judgeErasure(const ClearDtcsCounts& counts) {
  if (!counts.distSinceClearAfterKm || !counts.distSinceClearBeforeKm) return "unproven";
  if (*counts.distSinceClearAfterKm != 0) return "erased-nothing";
  return *counts.distSinceClearBeforeKm == 0 ? "unproven" : "erased";
}

std::string unprovenReason(const ClearDtcsCounts& counts) {
  if (counts.distSinceClearBeforeKm && *counts.distSinceClearBeforeKm == 0) {
    return "Distance since codes cleared already read 0 km before the press, so it cannot show a reset. Ride, then "
           "read the Faults tab.";
  }
  return "The counters could not be read back. Check the Faults tab in a minute.";
}

std::string countsSwept(const ClearDtcsCounts& counts) {
  if (!counts.storedBefore || !counts.storedAfter) return "";
  const int swept = *counts.storedBefore - *counts.storedAfter;
  const std::string tail = swept < 0 ? std::to_string(-swept) + " MORE than before" : std::to_string(swept) + " cleared";
  return ": " + std::to_string(*counts.storedBefore) + " stored → " + std::to_string(*counts.storedAfter) +
         " stored, " + tail;
}

const char* clearStatusWord(ClearDtcsOutcome::Status status) {
  switch (status) {
    case ClearDtcsOutcome::Status::kCleared:
      return "cleared";
    case ClearDtcsOutcome::Status::kRefused:
      return "refused";
    case ClearDtcsOutcome::Status::kFailed:
      return "failed";
  }
  return "failed";
}

// A CanTransmitter backed by a VcuSend, for the one-shot mode-03 reader.
class SendAdapter final : public obd::CanTransmitter {
 public:
  explicit SendAdapter(VcuSend send) : send_(std::move(send)) {}
  std::optional<std::string> transmit(std::uint32_t id, const obd::Payload& data) override { return send_(id, data); }

 private:
  VcuSend send_;
};

}  // namespace

// The whole clear-dtcs run: the frame handler during the action, and the sequence as a coroutine.
class ClearDtcsRun : public std::enable_shared_from_this<ClearDtcsRun> {
 public:
  ClearDtcsRun(VcuSend send, const ParameterNames& names, SecurityCooldown& cooldown, obd::SignalRecorder* recorder,
               obd::StoredCodes* stored, obd::ObdHold* hold, std::string directory, std::string versionLabel,
               std::function<ServiceGateVerdict()> gate, std::function<std::optional<std::string>()> refuseNow,
               std::function<double()> wallClockMs, std::function<PiClockEvidence()> clockEvidence,
               std::function<void(std::function<void(std::string)>)> setAbortable)
      : send_(std::move(send)),
        names_(names),
        cooldown_(cooldown),
        recorder_(recorder),
        stored_(stored),
        hold_(hold),
        directory_(std::move(directory)),
        versionLabel_(std::move(versionLabel)),
        gate_(std::move(gate)),
        refuseNow_(std::move(refuseNow)),
        wallClockMs_(std::move(wallClockMs)),
        clockEvidence_(std::move(clockEvidence)),
        setAbortable_(std::move(setAbortable)),
        adapter_(send_) {}

  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
    if (writeSession_ && writeSession_->handleFrame(id, data)) return true;
    if (pidPending_ && tryPidFrame(id, data)) return true;
    if (listReader_ && tryListFrame(id, data)) return true;
    return false;
  }

  async::Task<ServiceWriteAnswer> run() {
    const std::optional<obd::HoldTicket> ticket = co_await holdPoller();
    if (!ticket) co_return ServiceWriteAnswer{false, {}, obd::pollerRefusalFor(kWhat)};
    const std::optional<ServiceWriteAnswer> parked = refuseIfUnsafe("while the OBD poller parked");
    ServiceWriteAnswer answer = parked ? *parked : co_await clearOnParkedBus();
    if (hold_ != nullptr) hold_->release(*ticket);
    co_return answer;
  }

 private:
  std::int64_t loopNow() const { return async::currentLoop().now(); }
  obd::Now now() const { return {loopNow(), static_cast<std::int64_t>(wallClockMs_())}; }

  async::Task<std::optional<obd::HoldTicket>> holdPoller() {
    if (hold_ == nullptr) co_return std::nullopt;
    async::Deferred<bool> settled;
    const std::optional<obd::HoldTicket> ticket = hold_->request(
        kWhat, loopNow(), obd::ObdHold::kHoldWaitMs, obd::ObdHold::kMaxHoldMs,
        [settled](bool parked) mutable { settled.resolve(parked); });
    if (!ticket) co_return std::nullopt;
    const bool parked = co_await settled;
    co_return parked ? ticket : std::nullopt;
  }

  std::optional<ServiceWriteAnswer> refuseIfUnsafe(const std::string& when) {
    const std::optional<std::string> reason = refuseNow_();
    if (!reason) return std::nullopt;
    return ServiceWriteAnswer{false, {}, *reason + " — " + when + ". Nothing was sent."};
  }

  // --- one-shot PID poll ---------------------------------------------------------------------
  struct PidPending {
    int pid;
    async::Deferred<std::optional<std::array<std::uint8_t, 8>>> done;
    async::TimerId timer;
  };

  bool tryPidFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
    if (!pidPending_) return false;
    if (id < 0x7e0 || id > 0x7ef) return false;
    if (data.size() < 3 || (data[0] >> 4) != 0x0 || data[1] != 0x41 || data[2] != pidPending_->pid) return false;
    auto done = pidPending_->done;
    async::currentLoop().clearTimeout(pidPending_->timer);
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t at = 0; at < data.size() && at < 8; at++) bytes[at] = data[at];
    pidPending_.reset();
    done.resolve(bytes);
    return true;
  }

  // fileResponse: decode the PID we polled (only 0x01/0x31/0x02 are asked), record its signals.
  std::vector<std::pair<std::string, double>> fileResponse(int pid, const std::array<std::uint8_t, 8>& bytes) {
    const double a = bytes[3];
    const double b = bytes[4];
    std::vector<std::pair<std::string, double>> decoded;
    if (pid == 0x01) {
      const int byte = static_cast<int>(a);
      decoded.push_back({"mil_on", (byte & 0x80) != 0 ? 1 : 0});
      decoded.push_back({"dtc_count", static_cast<double>(byte & 0x7f)});
    } else if (pid == 0x31) {
      decoded.push_back({"dist_since_clear_km", 256 * a + b});
    } else if (pid == 0x02) {
      decoded.push_back({"freeze_frame_dtc", 256 * a + b});
    }
    for (const auto& [key, value] : decoded) {
      if (recorder_ != nullptr) recorder_->record(key, value);
    }
    if (pid == 0x02 && stored_ != nullptr) stored_->recordFreezeFrameDtc(static_cast<std::uint16_t>(256 * a + b));
    return decoded;
  }

  async::Task<std::optional<std::vector<std::pair<std::string, double>>>> pollPid(int pid) {
    async::Deferred<std::optional<std::array<std::uint8_t, 8>>> done;
    const std::array<std::uint8_t, 8> frame = {0x02, 0x01, static_cast<std::uint8_t>(pid), 0x55, 0x55, 0x55, 0x55, 0x55};
    auto self = shared_from_this();
    async::TimerId timer = async::currentLoop().setTimeout(obd::ObdPoller::kPidTimeoutMs, [self, done]() mutable {
      self->pidPending_.reset();
      done.resolve(std::nullopt);
    });
    pidPending_ = PidPending{pid, done, timer};
    if (const std::optional<std::string> problem = send_(0x7df, frame)) {
      async::currentLoop().clearTimeout(timer);
      pidPending_.reset();
      logError("obd", "send failed: %s", problem->c_str());
      done.resolve(std::nullopt);
    }
    const std::optional<std::array<std::uint8_t, 8>> reply = co_await done;
    if (!reply) co_return std::nullopt;
    co_return fileResponse(pid, *reply);
  }

  static std::optional<int> valueOf(const std::optional<std::vector<std::pair<std::string, double>>>& decoded,
                                    const std::string& key) {
    if (!decoded) return std::nullopt;
    for (const auto& [name, value] : *decoded) {
      if (name == key) return static_cast<int>(value);
    }
    return std::nullopt;
  }

  async::Task<CounterReading> readClearCounters() {
    const auto status = co_await pollPid(0x01);
    const auto distance = co_await pollPid(0x31);
    co_return CounterReading{valueOf(status, "dtc_count"), valueOf(distance, "dist_since_clear_km")};
  }

  // --- one-shot mode-03 list read (driving the trouble-code reader) --------------------------
  bool tryListFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
    if (listSettled_) return false;
    const bool consumed = listReader_->handleFrame(id, data, now());
    if (consumed) driveList();
    return consumed;
  }

  void driveList() {
    async::currentLoop().clearTimeout(listTimer_);
    if (listSettled_) return;
    if (const std::optional<obd::DtcReadOutcome> outcome = listReader_->takeOutcome()) {
      listSettled_ = true;
      listDone_.resolve(*outcome);
      return;
    }
    const std::optional<obd::Deadline> deadline = listReader_->nextDeadline();
    if (!deadline) return;  // not settled, no deadline — leave it; a frame will re-arm
    const std::int64_t wait = std::max<std::int64_t>(1, deadline->atMs - loopNow());
    auto self = shared_from_this();
    listTimer_ = async::currentLoop().setTimeout(wait, [self]() {
      self->listReader_->tick(self->now());
      self->driveList();
    });
  }

  async::Task<obd::DtcReadOutcome> runList() {
    listReader_ = std::make_unique<obd::TroubleCodeReader>(adapter_, listOrder_);
    listSettled_ = false;
    listDone_ = async::Deferred<obd::DtcReadOutcome>{};
    if (const std::optional<obd::DtcReadOutcome> immediate = listReader_->start(obd::kModeStoredDtcs, now())) {
      listSettled_ = true;
      obd::DtcReadOutcome result = *immediate;
      listReader_.reset();
      co_return result;
    }
    driveList();
    obd::DtcReadOutcome outcome = co_await listDone_;
    listReader_.reset();
    co_return outcome;
  }

  async::Task<CounterReading> readAfterCountersThenList(std::optional<int>& listedAfter) {
    const CounterReading counters = co_await readClearCounters();  // THE COUNTERS FIRST
    co_await pollPid(0x02);                                        // the freeze-frame code
    const obd::DtcReadOutcome outcome = co_await runList();
    if (stored_ != nullptr && recorder_ != nullptr) stored_->recordRead(outcome, obd::DtcList::kStored, now(), *recorder_);
    if (stored_ != nullptr) {
      const obd::TroubleCodeListState& list = stored_->list(obd::DtcList::kStored);
      if (list.state == obd::TroubleCodeListState::State::kCodes) listedAfter = static_cast<int>(list.codes.size());
    }
    co_return counters;
  }

  async::Task<ServiceWriteAnswer> clearOnParkedBus() {
    const CounterReading before = co_await readClearCounters();
    if (const std::optional<ServiceWriteAnswer> rolling = refuseIfUnsafe("while the counters were read")) {
      co_return *rolling;
    }
    logWarning(kTag, "about to send OBD Mode 04 — the stored trouble codes and the freeze frame will be erased");
    writeSession_ = std::make_shared<WriteSession>(send_, names_, cooldown_);
    setAbortable_([ws = writeSession_](std::string reason) { ws->abort(std::move(reason)); });
    const ClearDtcsOutcome outcome = co_await writeSession_->clearStoredDtcs();
    setAbortable_({});  // the actuating exchange is over; everything below is reads

    std::optional<ClearDtcsCounts> counts;
    std::string message;
    CounterReading after;
    if (outcome.status == ClearDtcsOutcome::Status::kCleared) {
      std::optional<int> listedAfter;
      after = co_await readAfterCountersThenList(listedAfter);
      ClearDtcsCounts built;
      built.storedBefore = before.storedCount;
      built.storedAfter = after.storedCount;
      built.distSinceClearBeforeKm = before.distSinceClearKm;
      built.distSinceClearAfterKm = after.distSinceClearKm;
      built.listedAfter = listedAfter;
      built.verdict = judgeErasure(built);
      counts = built;
      message = describeAcceptedClear(built);
    } else if (outcome.status == ClearDtcsOutcome::Status::kRefused) {
      message = "Refused: " + outcome.description + ".";
    } else {
      message = "Nothing confirmed: " + outcome.reason;
    }

    AuditRecord record;
    record.at = wallClockMs_();
    record.clockTrustworthy = checkPiClock(clockEvidence_()).trustworthy;
    record.action = AuditAction::kClearDtcs;
    record.status = clearStatusWord(outcome.status);
    record.before = AuditValue::of(describeCounters(before));
    record.after = counts ? AuditValue::of(describeCounters({counts->storedAfter, counts->distSinceClearAfterKm}))
                          : AuditValue::null();
    record.note = message;
    appendAuditRecord(directory_, std::move(record), versionLabel_);

    ServiceWriteResult result;
    result.action = AuditAction::kClearDtcs;
    result.status = clearStatusWord(outcome.status);
    result.message = message;
    result.succeeded =
        outcome.status == ClearDtcsOutcome::Status::kCleared && (!counts || judgeErasure(*counts) != "erased-nothing");
    if (counts) {
      result.hasClear = true;
      result.clear = *counts;
    }
    co_return ServiceWriteAnswer{true, result, {}};
  }

  std::string twiceBeforeAdvice() const {
    const std::string observation = "This has happened twice before, both times with a cable or a charge involved.";
    return verdictSeesACable(gate_())
               ? observation + " There is a cable in the inlet now — unplug it and try again."
               : observation;
  }

  std::string describeAcceptedClear(const ClearDtcsCounts& counts) const {
    const std::string listed =
        counts.listedAfter ? " The list now holds " + std::to_string(*counts.listedAfter) + "." : "";
    const std::string verdict = judgeErasure(counts);
    if (verdict == "unproven") {
      return "Mode 04 accepted, but nothing here proves the bike acted on it — a positive answer alone has twice meant "
             "nothing on this bike." +
             listed + " " + unprovenReason(counts);
    }
    if (verdict == "erased-nothing") {
      return "⚠️ Mode 04 was accepted AND THE BIKE ERASED NOTHING: distance since codes cleared still reads " +
             std::to_string(*counts.distSinceClearAfterKm) +
             " km, which a real clear resets to zero within half a second. " + twiceBeforeAdvice();
    }
    return "Cleared" + countsSwept(counts) + ". Distance since codes cleared went " +
           std::to_string(*counts.distSinceClearBeforeKm) + " km → 0 km, which is the proof the bike really erased its "
           "fault memory." +
           listed + " Codes whose faults are still active come straight back.";
  }

  VcuSend send_;
  const ParameterNames& names_;
  SecurityCooldown& cooldown_;
  obd::SignalRecorder* recorder_;
  obd::StoredCodes* stored_;
  obd::ObdHold* hold_;
  std::string directory_;
  std::string versionLabel_;
  std::function<ServiceGateVerdict()> gate_;
  std::function<std::optional<std::string>()> refuseNow_;
  std::function<double()> wallClockMs_;
  std::function<PiClockEvidence()> clockEvidence_;
  std::function<void(std::function<void(std::string)>)> setAbortable_;
  SendAdapter adapter_;

  std::shared_ptr<WriteSession> writeSession_;
  std::optional<PidPending> pidPending_;
  obd::DeadlineOrder listOrder_;
  std::unique_ptr<obd::TroubleCodeReader> listReader_;
  async::Deferred<obd::DtcReadOutcome> listDone_;
  async::TimerId listTimer_;
  bool listSettled_ = false;
};

async::Task<ServiceWriteAnswer> VcuWriteRunner::performClearDtcs() {
  auto run = std::make_shared<ClearDtcsRun>(
      context_.send, names(), cooldown_, context_.recorder, context_.stored, context_.hold, context_.directory,
      context_.version.label, context_.gate, [this] { return refusalFor(ServiceWriteRequest::Kind::kClearDtcs); },
      context_.wallClockMs, context_.clockEvidence,
      [this](std::function<void(std::string)> abort) { abortRunning_ = std::move(abort); });
  runningFrame_ = [run](std::uint32_t id, std::span<const std::uint8_t> data) { return run->handleFrame(id, data); };
  abortRunning_ = {};
  co_return co_await run->run();
}

}  // namespace emcool::vcu
