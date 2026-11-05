#pragma once

// Service mode's WRITE engine, ported from cool-eva src/vcu/write-runner.ts and the /vcu-write
// handler: decide whether the bike may be changed (SERVICE_WRITE_ENABLED, the bus lease, the
// safety gate, the table-type gate, the allowlist), do exactly one thing, read it back, and write
// down what happened. A separate switch from the read runner, because the two are not the same
// risk. Stage 4 carries every write action except charge-current/charge-stop (stage 5).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "emcool/async/task.hpp"
#include "emcool/obd/obd_hold.hpp"
#include "emcool/obd/stored_codes.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/charge_ack_watch.hpp"
#include "emcool/vcu/runner.hpp"  // BusLease, ServiceContext-adjacent helpers
#include "emcool/vcu/service_gate.hpp"
#include "emcool/vcu/snapshot.hpp"
#include "emcool/vcu/store.hpp"
#include "emcool/vcu/table_gate.hpp"
#include "emcool/vcu/write_audit.hpp"
#include "emcool/vcu/write_session.hpp"

namespace emcool::vcu {

// Which commit/firmware is running (cool-eva RunningVersion). emcool fills it from the IDF app
// description; the differential harness normalises it, so only the shape is load-bearing here.
struct RunningVersion {
  std::optional<std::string> commit;
  bool dirty = false;
  bool trustworthy = false;
  std::string label;
};

// What the write runner can be asked to do. Closed; charge-current/charge-stop are stage 5.
struct ServiceWriteRequest {
  enum class Kind : std::uint8_t {
    kParameter,
    kParameters,
    kBit,
    kReadServiceStamp,
    kSetServicePoint,
    kSyncClock,
    kClearDtcs,
    kChargeCurrent,
    kChargeStop,
    kChargeSocLimit,
    kChargeSocLimitRead,
    kResetVcu,
  };
  enum class ChargeOrigin : std::uint8_t { kManual, kAutomatic };
  struct BatchWrite {
    std::string name;
    double value = 0;
    double expectedCurrent = 0;
  };
  Kind kind = Kind::kReadServiceStamp;
  std::string name;              // parameter, bit
  double value = 0;              // parameter
  double expectedCurrent = 0;    // parameter, bit
  std::string bit;               // bit
  bool on = false;               // bit
  std::vector<BatchWrite> writes;  // parameters
  int percent = 0;               // charge-soc-limit
  int amps = 0;                  // charge-current
  ChargeOrigin origin = ChargeOrigin::kManual;  // charge-current
};

const char* serviceWriteKindName(ServiceWriteRequest::Kind kind);

// What a parameter reads on the bike at the end of an action, or null.
struct ServiceWriteOnBike {
  std::string name;
  double value = 0;
  std::optional<std::string> rawHex;
};

// How one write came out, in the shape the page renders.
struct PerWriteResult {
  std::string name;
  std::string status;
  std::string message;
  bool succeeded = false;
  std::optional<ServiceWriteOnBike> onBike;
};

struct ClearDtcsCounts {
  std::optional<int> storedBefore;
  std::optional<int> storedAfter;
  std::optional<int> distSinceClearBeforeKm;
  std::optional<int> distSinceClearAfterKm;
  std::optional<int> listedAfter;
  std::string verdict;  // erased | erased-nothing | unproven
};

struct ServiceWriteResult {
  AuditAction action = AuditAction::kParameterWrite;
  std::string status;
  std::string message;
  bool succeeded = false;
  bool hasStamp = false;
  std::optional<ServiceStamp> stampBefore;
  std::optional<ServiceStamp> stampAfter;
  bool onBikePresent = false;  // the field is present (may be null)
  std::optional<ServiceWriteOnBike> onBike;
  bool hasWrites = false;
  std::vector<PerWriteResult> writes;
  bool hasClear = false;
  ClearDtcsCounts clear;
};

struct ServiceWriteAnswer {
  bool ok = false;
  ServiceWriteResult result;  // ok
  std::string reason;         // !ok
  // !ok on a charge-current only: a frame may already have left (cool-eva's mayHaveSent). The 0x120
  // commit twin goes first, so a later failure must never read as "not sent".
  bool mayHaveSent = false;
};

// cool-eva's VcuWriteResponse.chargeCurrentAttempt: on a charge-current that produced no result,
// whether nothing went out or delivery is uncertain. Absent for every other action.
enum class ChargeCurrentAttempt : std::uint8_t { kNone, kNotSent, kUnknown };

// What the three gates do about one action.
struct ServiceActionPolicy {
  bool bikeStateGateApplies = false;
  bool refusedWhileCharging = false;
  bool tableGateApplies = false;
};
ServiceActionPolicy serviceActionPolicy(ServiceWriteRequest::Kind kind);
// The shipped refusal for one action, or nullopt. `read` samples a gate signal.
std::optional<std::string> serviceActionRefusal(const ServiceActionPolicy& policy, const ServiceGateVerdict& gate,
                                                const GateReader& read, ServiceWriteRequest::Kind kind);

// Parse a /vcu-write query into one action, or a reason. Pure. `nowMs` is for the sync-clock window.
struct WriteRequestParse {
  bool ok = false;
  ServiceWriteRequest request;
  std::string reason;
};
WriteRequestParse parseWriteRequest(const std::function<std::optional<std::string>(std::string_view)>& get,
                                    const std::function<std::vector<std::string>(std::string_view)>& getAll,
                                    double nowMs);
// "2026-08-16T14:03Z".
std::string utcMinute(double epochMs);

// What the page asked to be told.
struct WriteStatusRequest {
  std::optional<std::string> detailFor;
  bool includeList = true;
};
WriteStatusRequest parseStatusRequest(const std::function<std::optional<std::string>(std::string_view)>& get);

// Everything the write runner needs from the firmware.
struct WriteContext {
  VcuSend send;
  bool enabled = false;
  bool busIsActive = false;
  std::string directory;
  std::function<ServiceGateVerdict()> gate;
  GateReader readSignal;
  std::function<double()> wallClockMs;
  std::function<std::int64_t()> monotonicMs;  // for the charge-ack window (cool-eva's monotonicNow)
  // GPS evidence for the clock verdict: the gps_epoch_s value and its monotonic age.
  std::function<PiClockEvidence()> clockEvidence;
  // The last sweep's TABLE_TYPE report, sampled per attempt; nullopt when none.
  std::function<std::optional<TableTypeReport>()> latestReport;
  RunningVersion version;
  obd::ObdHold* hold = nullptr;         // the always-on poller's hold; null with OBD off
  obd::SignalRecorder* recorder = nullptr;  // for clear-dtcs's PID records
  obd::StoredCodes* stored = nullptr;   // for clear-dtcs's mode-03 read-back into /stored-dtcs
  VcuStore* store = nullptr;            // the snapshot store, for swept values and the table report
  ChargeAckWatch* ackWatch = nullptr;   // the charge-current ack readout; null until stage 5 wired
  // Told BEFORE a charge current reaches the bus, with where it came from — the automatic
  // controller stands down for a manual one. A no-op until the auto controller is ported.
  std::function<void(int amps, ServiceWriteRequest::ChargeOrigin origin)> onChargeCurrentOutgoing;
};

class VcuWriteRunner {
 public:
  VcuWriteRunner(WriteContext context, BusLease& lease) : context_(std::move(context)), lease_(lease) {}

  async::Task<ServiceWriteAnswer> perform(ServiceWriteRequest request);
  bool handleCanFrame(std::uint32_t id, std::span<const std::uint8_t> data);
  void stop();

  // The /vcu-write body: {status, result, message[, chargeCurrentAttempt]}. `result`/`message` null
  // unless given.
  void writeResponseJson(text::TextBuffer& out, const WriteStatusRequest& request, const ServiceWriteResult* result,
                         const std::optional<std::string>& message,
                         ChargeCurrentAttempt attempt = ChargeCurrentAttempt::kNone);

  SecurityCooldown& cooldown() { return cooldown_; }

 private:
  struct Ready {
    bool ok = false;
    std::optional<TableTypeReport> tableType;
    std::optional<BusLease::Ticket> lease;
    std::string reason;
  };
  std::optional<std::string> refusalFor(ServiceWriteRequest::Kind kind);
  async::Task<Ready> checkPreconditions(const ServiceWriteRequest& request);
  async::Task<ServiceWriteAnswer> performOnBus(const ServiceWriteRequest& request,
                                               const std::optional<TableTypeReport>& tableType);
  async::Task<ServiceWriteAnswer> performParameterWrite(const ServiceWriteRequest& request,
                                                        const std::optional<TableTypeReport>& tableType);
  async::Task<ServiceWriteAnswer> performParameterWrites(const ServiceWriteRequest& request,
                                                         const std::optional<TableTypeReport>& tableType);
  async::Task<ServiceWriteAnswer> performReadStamp();
  async::Task<ServiceWriteAnswer> performServicePoint();
  async::Task<ServiceWriteAnswer> performClockSync();
  async::Task<ServiceWriteAnswer> performResetVcu();
  async::Task<ServiceWriteAnswer> performClearDtcs();
  async::Task<ServiceWriteAnswer> performChargeCurrent(const ServiceWriteRequest& request);
  async::Task<ServiceWriteAnswer> performChargeStop();
  async::Task<ServiceWriteAnswer> performChargeSocLimit(int percent);
  async::Task<ServiceWriteAnswer> performChargeSocLimitRead();

  void audit(AuditRecord record);
  const ParameterNames& names() const { return context_.store->names(); }
  async::TimerId startGateWatchdog();
  void writeStatusJson(text::TextBuffer& out, const WriteStatusRequest& request);
  // What the last recorded sweep found for one allowlisted parameter, or nullopt.
  struct SweptValue {
    double value;
    std::optional<std::string> rawHex;
    std::string label;
    double readAt;
    bool complete;
  };
  std::optional<SweptValue> sweptValueFor(const WriteTarget& target);

  WriteContext context_;
  BusLease& lease_;
  SecurityCooldown cooldown_;
  std::shared_ptr<WriteSession> running_;
  std::function<bool(std::uint32_t, std::span<const std::uint8_t>)> runningFrame_;
  std::function<void(std::string)> abortRunning_;
};

}  // namespace emcool::vcu
