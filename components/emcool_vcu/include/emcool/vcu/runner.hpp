#pragma once

// Service mode's reads, ported from cool-eva src/vcu/sweep.ts, probe.ts, read-runner.ts,
// bus-lease.ts and sweep-targets.ts:
//   - a sweep reads every parameter the table and the A8's firmware table name, resuming whatever
//     an unfinished sweep already read, and writes a snapshot
//   - a probe reads one identifier, any bank, off either micro
//   - the runner refuses either unless the bike is safe to service and the bus is free, keeps
//     checking the bike while it works (every 200 ms), and stops the moment it isn't safe
// One thing at a time on the bus (the lease); a probe also parks the OBD poller.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/async/task.hpp"
#include "emcool/obd/obd_hold.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/kwp_client.hpp"
#include "emcool/vcu/readings.hpp"
#include "emcool/vcu/service_gate.hpp"
#include "emcool/vcu/store.hpp"

namespace emcool::vcu {

// What service mode needs from the rest of the firmware.
struct ServiceContext {
  VcuSend send;
  // A gate signal's latest value and its age, from live state.
  GateReader readSignal;
  std::function<double()> wallClockMs;
  // The OBD poller's hold; null with OBD off.
  obd::ObdHold* hold = nullptr;
  VcuStore* store = nullptr;
};

ServiceGateVerdict readGate(const ServiceContext& context);

// Asks the poller to park for `reason`, and waits until it has or won't: the ticket, or nullopt.
async::Task<std::optional<obd::HoldTicket>> holdObdPoller(ServiceContext& context, std::string reason);

// cool-eva bus-lease.ts: one thing at a time on the bus.
class BusLease {
 public:
  using Ticket = std::uint32_t;
  std::optional<Ticket> acquire(std::string name);
  void release(Ticket ticket);
  const std::string* heldBy() const { return holder_ ? &holder_->second : nullptr; }

 private:
  std::optional<std::pair<Ticket, std::string>> holder_;
  Ticket next_ = 0;
};

struct SweepTarget {
  Micro micro;
  int index;
  bool widthUnverified;  // an A8 firmware-table row: its width isn't in params.ecf
};
// A9 first, then A8, each by index.
std::vector<SweepTarget> sweepTargets(const ParameterNames& names);
inline constexpr Micro kMicros[] = {Micro::kA9, Micro::kA8};

struct SweepResult {
  SnapshotHeader snapshot;
  std::optional<std::string> stoppedBecause;
};

class ParameterSweep {
 public:
  explicit ParameterSweep(ServiceContext& context);
  void start();
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) {
    return client_.handleFrame(id, data, arrival);
  }
  void abort(std::string reason);
  const std::map<int, RowSummary>& rows() const { return rows_; }
  int expected() const { return expected_; }
  const async::Deferred<SweepResult>& finished() const { return finished_; }

 private:
  async::Task<> run();
  async::Task<VcuReadOutcome> readOneTarget(const SweepTarget& target);
  async::Task<> pingMicros();
  bool mayContinue();

  ServiceContext& context_;
  VcuKwpClient client_;
  std::map<int, RowSummary> rows_;
  std::optional<std::string> stoppedBecause_;
  std::optional<std::string> pollerRefusal_;
  std::map<int, VcuParameterRow> tableTypeRows_;
  int expected_;
  async::Deferred<SweepResult> finished_;
};

struct VcuProbeRequest {
  Micro target;
  int bank;
  int index;
};
// target, bank and index as the dashboard sends them; or why not.
struct ProbeParse {
  std::optional<VcuProbeRequest> request;
  std::string reason;
};
ProbeParse parseProbeRequest(std::optional<std::string_view> target, std::optional<std::string_view> bank,
                             std::optional<std::string_view> index);

struct VcuProbeReading {
  VcuProbeRequest request;
  int identifier;
  ReadStatus status;
  std::optional<std::string> name, section;
  std::optional<ArrivalLatency> flowControlLatency;
  std::optional<std::string> rawHex;
  std::optional<double> unsignedValue, signedValue, value;
  std::optional<std::string> note;
};
VcuProbeReading describeProbe(const VcuReadOutcome& outcome, const ParameterNames& names);
void writeProbeReadingJson(text::TextBuffer& out, const VcuProbeReading& reading);

// What a probe came to: a reading, or why there wasn't one.
struct ProbeOutcome {
  std::optional<VcuProbeReading> reading;
  std::string reason;
};
struct LifetimeOutcome {
  std::optional<LifetimeReadResult> result;
  std::string reason;
};
struct FreezeFrameOutcome {
  std::optional<FreezeFrameReadResult> result;
  std::string reason;
};

class VcuReadRunner {
 public:
  VcuReadRunner(ServiceContext& context, BusLease& lease, bool busIsActive)
      : context_(context), lease_(lease), busIsActive_(busIsActive) {}

  struct Started {
    bool started;
    std::optional<std::string> reason;
  };
  Started start();
  bool cancel();
  ServiceGateVerdict gate() const { return readGate(context_); }
  async::Task<ProbeOutcome> probe(VcuProbeRequest request);
  async::Task<LifetimeOutcome> readLifetimeStatistics();
  async::Task<FreezeFrameOutcome> readFreezeFrames();
  // A frame in 0x7E0-0x7EF: the running sweep's or probe's, if it's theirs.
  bool handleCanFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival);
  void stop();

  // cool-eva VcuReadState, as JSON.
  void writeStateJson(text::TextBuffer& out) const;
  bool running() const { return sweep_ != nullptr; }

 private:
  struct Tally {
    int total = 0;
    int read = 0;
    std::map<ReadStatus, int> byStatus;
    int microRead[2] = {0, 0};
    int microFailed[2] = {0, 0};
    bool microSeen[2] = {false, false};
  };
  static Tally tallyOf(const std::map<int, RowSummary>& rows);
  static void writeTallyJson(text::TextBuffer& out, const Tally& tally);

  std::optional<std::string> checkBusFreeRefusals() const;
  // cool-eva runOneShotBusModule(): the checks, the poller parked, the lease, the watchdog, then the
  // read; nullopt and why if it couldn't run.
  template <typename Result, typename Module>
  async::Task<std::pair<std::optional<Result>, std::string>> runOneShot(std::string what,
                                                                        std::function<std::shared_ptr<Module>()> make);
  async::Task<> watchSweep(std::shared_ptr<ParameterSweep> sweep);
  async::TimerId startWatchdog(std::function<void(std::string)> onUnsafe);
  void stopGateWatchdog();

  ServiceContext& context_;
  BusLease& lease_;
  const bool busIsActive_;
  std::shared_ptr<ParameterSweep> sweep_;
  std::optional<BusLease::Ticket> leaseTicket_;
  // A one-shot read in progress: its name, and what to hand frames to and abort.
  std::optional<std::string> oneShotName_;
  std::shared_ptr<OneShotRead> oneShot_;
  std::optional<double> startedAt_, finishedAt_;
  std::optional<std::string> failure_;
  std::optional<Tally> lastTally_;
  bool lastComplete_ = false;
  async::TimerId watchdog_;
};

}  // namespace emcool::vcu
