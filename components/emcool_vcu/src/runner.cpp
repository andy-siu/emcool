#include "emcool/vcu/runner.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "emcool/log.hpp"
#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr std::int64_t kGateWatchIntervalMs = 200;

std::string joinBlockers(const std::vector<std::string>& blockers) {
  std::string text;
  for (const std::string& blocker : blockers) {
    if (!text.empty()) text += "; ";
    text += blocker;
  }
  return text;
}

int microSlot(Micro micro) { return micro == Micro::kA8 ? 0 : 1; }

VcuReadOutcome notSent(const SweepTarget& target, std::string reason) {
  VcuReadOutcome outcome;
  outcome.micro = target.micro;
  outcome.index = target.index;
  outcome.identifier = identifierFor(kCalibrationBank, target.index).value_or(0);
  outcome.status = ReadStatus::kNotSent;
  outcome.reason = std::move(reason);
  return outcome;
}

const std::string& pollerRefusal() {
  static const std::string refusal = obd::pollerRefusalFor("the A8 rows whose width comes from the firmware table");
  return refusal;
}

// One microtask hop: where cool-eva's sweep first awaits its file reads.
struct Yield {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> caller) { async::detail::resumeLater(caller); }
  void await_resume() const noexcept {}
};

// JavaScript's Number(text) for the forms a dashboard field might hold; nullopt for NaN.
std::optional<double> jsNumberParse(std::string_view text) {
  if (text.empty()) return 0.0;
  if (text.size() > 2 && text[0] == '0' && std::strchr("xXoObB", text[1]) != nullptr) {
    const int base = text[1] == 'x' || text[1] == 'X' ? 16 : text[1] == 'o' || text[1] == 'O' ? 8 : 2;
    double value = 0;
    for (const char character : text.substr(2)) {
      const int digit = std::isdigit(static_cast<unsigned char>(character)) ? character - '0'
                        : std::isxdigit(static_cast<unsigned char>(character))
                            ? std::tolower(static_cast<unsigned char>(character)) - 'a' + 10
                            : 99;
      if (digit >= base) return std::nullopt;
      value = value * base + digit;
    }
    return value;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size()) return std::nullopt;
  // strtod takes "inf", "nan" and hex floats, which Number() spells differently or not at all.
  for (const char character : copy) {
    if (std::isalpha(static_cast<unsigned char>(character)) && character != 'e' && character != 'E') return std::nullopt;
  }
  return value;
}

std::optional<double> parseNumber(std::optional<std::string_view> raw) {
  if (!raw) return std::nullopt;
  std::string_view text = *raw;
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (text.empty()) return std::nullopt;
  const std::optional<double> value = jsNumberParse(text);
  if (!value || std::floor(*value) != *value || std::isinf(*value)) return std::nullopt;
  return value;
}

std::string describeProbeFailure(const VcuReadOutcome& outcome) {
  switch (outcome.status) {
    case ReadStatus::kRefused:
      return "refused with NRC " + outcome.description + " — the ECU is there and will not serve this identifier";
    case ReadStatus::kNoResponse:
      return "a session was open and this identifier got silence — not the same claim as “no such identifier”";
    case ReadStatus::kNoSession:
      return outcome.reason + " — either nothing is at this address, or it is asleep";
    case ReadStatus::kStalled:
      return outcome.reason + " — not the same claim as silence";
    case ReadStatus::kAbandoned:
      return "the reply was discarded rather than decoded: " + outcome.reason;
    case ReadStatus::kUnrecognised:
      return outcome.reason;
    case ReadStatus::kNotSent:
      return "never asked — " + outcome.reason;
    case ReadStatus::kRead:
      break;
  }
  return readStatusName(outcome.status);
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

void writeOptionalNumber(text::TextBuffer& out, const std::optional<double>& value) {
  if (value) {
    out.appendNumber(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

ServiceGateVerdict readGate(const ServiceContext& context) {
  return evaluateServiceGate(sampleServiceGate(context.readSignal));
}

async::Task<std::optional<obd::HoldTicket>> holdObdPoller(ServiceContext& context, std::string reason) {
  if (context.hold == nullptr) co_return std::nullopt;
  async::Deferred<bool> settled;
  const std::optional<obd::HoldTicket> ticket =
      context.hold->request(reason, async::currentLoop().now(), obd::ObdHold::kHoldWaitMs, obd::ObdHold::kMaxHoldMs,
                            [settled](bool parked) mutable { settled.resolve(parked); });
  if (!ticket) co_return std::nullopt;
  const bool parked = co_await settled;
  co_return parked ? ticket : std::nullopt;
}

std::optional<BusLease::Ticket> BusLease::acquire(std::string name) {
  if (holder_) return std::nullopt;
  holder_.emplace(++next_, std::move(name));
  return holder_->first;
}

void BusLease::release(Ticket ticket) {
  if (holder_ && holder_->first == ticket) holder_.reset();
}

std::vector<SweepTarget> sweepTargets(const ParameterNames& names) {
  std::vector<SweepTarget> targets;
  for (const VcuParameter& parameter : names.active().parameters) {
    targets.push_back({parameter.micro, parameter.index, false});
  }
  for (const A8FirmwareRow& row : a8FirmwareRows()) targets.push_back({Micro::kA8, row.index, true});
  std::stable_sort(targets.begin(), targets.end(), [](const SweepTarget& left, const SweepTarget& right) {
    const int leftMicro = left.micro == Micro::kA9 ? 0 : 1;
    const int rightMicro = right.micro == Micro::kA9 ? 0 : 1;
    return leftMicro != rightMicro ? leftMicro < rightMicro : left.index < right.index;
  });
  return targets;
}

// --- The sweep ----------------------------------------------------------------------------------

ParameterSweep::ParameterSweep(ServiceContext& context)
    : context_(context),
      client_(context.send),
      expected_(static_cast<int>(sweepTargets(context.store->names()).size())) {}

void ParameterSweep::start() { run().detach(); }

void ParameterSweep::abort(std::string reason) {
  if (!stoppedBecause_) stoppedBecause_ = std::move(reason);
  client_.stop();
}

bool ParameterSweep::mayContinue() {
  if (stoppedBecause_) return false;
  const ServiceGateVerdict verdict = readGate(context_);
  if (verdict.safe) return true;
  const std::string blockers = joinBlockers(verdict.blockers);
  logWarning("vcu-sweep", "leaving service mode — %s", blockers.c_str());
  abort("the bike stopped being safe to service — " + blockers);
  return false;
}

async::Task<> ParameterSweep::pingMicros() {
  for (const Micro micro : kMicros) {
    if (!mayContinue()) co_return;
    const bool reachable = co_await client_.ping(micro);
    logInfo("vcu-sweep", "%s %s", microName(micro), reachable ? "session open, responding" : "NOT responding to 10 81 + 3E");
  }
}

async::Task<VcuReadOutcome> ParameterSweep::readOneTarget(const SweepTarget& target) {
  if (!target.widthUnverified) co_return co_await client_.readParameter(target.micro, target.index);
  if (pollerRefusal_) co_return notSent(target, *pollerRefusal_);
  const std::string what = std::string("a parameter sweep's read of ") + microName(target.micro) + " index " +
                           std::to_string(target.index);
  const std::optional<obd::HoldTicket> ticket = co_await holdObdPoller(context_, what);
  if (ticket) {
    VcuReadOutcome outcome = !mayContinue() ? notSent(target, stoppedBecause_.value_or(pollerRefusal()))
                                            : co_await client_.readParameter(target.micro, target.index);
    context_.hold->release(*ticket);
    co_return outcome;
  }
  logWarning("vcu-sweep", "%s", obd::pollerRefusalFor(what).c_str());
  pollerRefusal_ = pollerRefusal();
  co_return notSent(target, pollerRefusal());
}

async::Task<> ParameterSweep::run() {
  // cool-eva's sweep awaits its file reads first, so the runner finishes starting it before it moves.
  co_await Yield{};
  VcuStore& store = *context_.store;
  ParameterNames& names = store.names();
  const std::vector<SweepTarget> targets = sweepTargets(names);
  rows_ = store.loadPartialRows(&tableTypeRows_);
  std::vector<SweepTarget> remaining;
  for (const SweepTarget& target : targets) {
    const auto found = rows_.find(target.index);
    if (found == rows_.end() || found->second.status != ReadStatus::kRead) remaining.push_back(target);
  }
  logInfo("vcu-sweep", "reading %zu parameter(s) (%zu already on record from an earlier run)", remaining.size(),
          rows_.size());
  std::optional<VcuStore::PartialLog> partial = store.openPartialLog();
  co_await pingMicros();
  for (const SweepTarget& target : remaining) {
    if (!mayContinue()) break;
    const VcuReadOutcome outcome = co_await readOneTarget(target);
    if (stoppedBecause_) break;
    const VcuParameterRow row = toParameterRow(outcome, names);
    rows_[row.index] = {row.micro, row.status};
    if (isTableTypeIndex(row.index)) tableTypeRows_.insert_or_assign(row.index, row);
    if (partial) partial->append(row, context_.wallClockMs());
    logInfo("vcu-sweep", "%s", describeRow(row).c_str());
  }
  const long startOffset = partial ? partial->startOffset() : 0;
  if (partial) partial->close();
  client_.stop();

  bool complete = !stoppedBecause_;
  for (const SweepTarget& target : targets) complete = complete && rows_.count(target.index) > 0;
  SnapshotHeader header{context_.wallClockMs(), complete, {Micro::kA9, Micro::kA8}};

  // cool-eva reportTableTypeToConsole(), on the rows as read.
  const TableTypeReport report = reportTableType(
      [&](int index) -> const VcuParameterRow* {
        const auto found = tableTypeRows_.find(index);
        return found == tableTypeRows_.end() ? nullptr : &found->second;
      },
      names);
  for (const std::string& line : report.lines) {
    if (report.mismatched || report.split || !report.unusable.empty()) {
      logError("vcu-sweep", "%s", line.c_str());
    } else if (report.confirmed) {
      logInfo("vcu-sweep", "%s", line.c_str());
    } else {
      logWarning("vcu-sweep", "%s", line.c_str());
    }
  }
  const int active = names.active().tableType;
  if (report.tableType && *report.tableType != active) {
    const std::vector<int> twins = contentTwinsOf(static_cast<int>(*report.tableType));
    if (std::find(twins.begin(), twins.end(), active) == twins.end()) {
      logWarning("vcu-sweep",
                 "⚠️  the NAMES printed above are %s's, but this bike runs %s — the snapshot on disk and "
                 "/params.html are re-named from the bike's own table, this scrollback is not. Read it again there "
                 "if a name matters.",
                 describeTableType(active).c_str(), describeTableType(*report.tableType).c_str());
    }
  }
  store.writeSnapshot(header, startOffset);
  finished_.resolve({header, stoppedBecause_});
}

// --- Probes -------------------------------------------------------------------------------------

ProbeParse parseProbeRequest(std::optional<std::string_view> target, std::optional<std::string_view> bank,
                             std::optional<std::string_view> index) {
  std::optional<Micro> micro;
  if (target) {
    std::string upper(*target);
    for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (upper == "A9") micro = Micro::kA9;
    if (upper == "A8") micro = Micro::kA8;
  }
  if (!micro) return {std::nullopt, "target must be one of A9, A8, not " + std::string(target.value_or("(nothing)"))};
  const std::optional<double> bankNumber = parseNumber(bank);
  if (!bankNumber || *bankNumber < 0 || *bankNumber > 0xf) {
    return {std::nullopt, "bank must be a whole number 0…15, not " + std::string(bank.value_or("(nothing)"))};
  }
  const std::optional<double> indexNumber = parseNumber(index);
  if (!indexNumber || *indexNumber < 0 || *indexNumber > 0x0fff) {
    return {std::nullopt, "index must be a whole number 0…4095, not " + std::string(index.value_or("(nothing)"))};
  }
  return {VcuProbeRequest{*micro, static_cast<int>(*bankNumber), static_cast<int>(*indexNumber)}, {}};
}

VcuProbeReading describeProbe(const VcuReadOutcome& outcome, const ParameterNames& names) {
  const VcuParameter* parameter = outcome.bank == kCalibrationBank ? names.atIndex(outcome.index) : nullptr;
  const A8FirmwareRow* firmware = parameter != nullptr ? nullptr : firmwareRowFor(outcome.micro, outcome.bank, outcome.index);
  VcuProbeReading reading;
  reading.request = {outcome.micro, outcome.bank, outcome.index};
  reading.identifier = outcome.identifier;
  reading.status = outcome.status;
  if (parameter != nullptr) {
    reading.name = std::string(parameter->name());
    if (parameter->section() != nullptr) reading.section = std::string(parameter->section());
  }
  reading.flowControlLatency = outcome.flowControlLatency;
  const std::optional<std::string> known =
      firmware != nullptr ? std::optional<std::string>(describeFirmwareRow(*firmware)) : std::nullopt;
  if (outcome.status != ReadStatus::kRead) {
    reading.note = note({describeProbeFailure(outcome), known});
    return reading;
  }
  std::optional<RecordEncoding> encoding;
  if (parameter != nullptr) encoding = RecordEncoding{parameter->type, parameter->isSigned, nullptr};
  else if (firmware != nullptr) encoding = RecordEncoding{firmware->type, std::nullopt, "A8's firmware table"};
  const VcuParameterValue interpreted = interpretRecord(outcome.record, encoding);
  reading.rawHex = interpreted.rawHex;
  reading.unsignedValue = interpreted.unsignedValue;
  reading.signedValue = interpreted.signedValue;
  reading.value = interpreted.value;
  if (interpreted.widthMismatch && encoding) {
    reading.note = note({describeWidthMismatch(outcome.record.size(), *encoding), known});
  } else if (parameter != nullptr) {
    reading.note.reset();
  } else if (known) {
    reading.note = known;
  } else {
    reading.note = "nothing in the name table describes this identifier — the bytes are real, their width and sign are "
                   "not known";
  }
  return reading;
}

void writeProbeReadingJson(text::TextBuffer& out, const VcuProbeReading& reading) {
  out.append("{\"target\":");
  out.appendJsonString(microName(reading.request.target));
  out.append(",\"bank\":");
  out.appendInteger(reading.request.bank);
  out.append(",\"index\":");
  out.appendInteger(reading.request.index);
  out.append(",\"identifier\":");
  out.appendInteger(reading.identifier);
  out.append(",\"status\":");
  out.appendJsonString(readStatusName(reading.status));
  out.append(",\"name\":");
  writeOptionalString(out, reading.name);
  out.append(",\"section\":");
  writeOptionalString(out, reading.section);
  out.append(",\"flowControlLatency\":");
  writeLatencyJson(out, reading.flowControlLatency);
  out.append(",\"rawHex\":");
  writeOptionalString(out, reading.rawHex);
  out.append(",\"unsigned\":");
  writeOptionalNumber(out, reading.unsignedValue);
  out.append(",\"signed\":");
  writeOptionalNumber(out, reading.signedValue);
  out.append(",\"value\":");
  writeOptionalNumber(out, reading.value);
  out.append(",\"note\":");
  writeOptionalString(out, reading.note);
  out.append("}");
}

// --- The runner ---------------------------------------------------------------------------------

std::optional<std::string> VcuReadRunner::checkBusFreeRefusals() const {
  if (sweep_) return std::string("a parameter read is already running");
  if (oneShotName_) return *oneShotName_ + " is already running";
  if (!busIsActive_) {
    return std::string(
        "the bus is listen-only (OBD_ENABLED=0) — nothing can be transmitted, so every read would time out");
  }
  const ServiceGateVerdict verdict = readGate(context_);
  if (!verdict.safe) return "the bike is not safe to service — " + joinBlockers(verdict.blockers);
  if (const std::string* holder = lease_.heldBy()) return *holder + " is using the bus — one thing at a time";
  return std::nullopt;
}

async::TimerId VcuReadRunner::startWatchdog(std::function<void(std::string)> onUnsafe) {
  auto fired = std::make_shared<bool>(false);
  return async::currentLoop().setInterval(kGateWatchIntervalMs, [this, fired, onUnsafe] {
    if (*fired) return;
    const ServiceGateVerdict verdict = readGate(context_);
    if (verdict.safe) return;
    *fired = true;
    const std::string blockers = joinBlockers(verdict.blockers);
    logWarning("vcu-read", "leaving service mode — %s", blockers.c_str());
    onUnsafe("the bike stopped being safe to service — " + blockers);
  });
}

void VcuReadRunner::stopGateWatchdog() {
  async::currentLoop().clearTimeout(watchdog_);
  watchdog_ = {};
}

VcuReadRunner::Started VcuReadRunner::start() {
  if (const std::optional<std::string> refusal = checkBusFreeRefusals()) return {false, refusal};
  const std::optional<BusLease::Ticket> ticket = lease_.acquire("a parameter read");
  if (!ticket) return {false, *lease_.heldBy() + " is using the bus — one thing at a time"};
  leaseTicket_ = ticket;
  std::shared_ptr<ParameterSweep> sweep = std::make_shared<ParameterSweep>(context_);
  sweep->start();
  sweep_ = sweep;
  startedAt_ = context_.wallClockMs();
  finishedAt_.reset();
  failure_.reset();
  lastTally_.reset();
  lastComplete_ = false;
  stopGateWatchdog();
  watchdog_ = startWatchdog([this, sweep](std::string reason) {
    sweep->abort(std::move(reason));
    stopGateWatchdog();
  });
  watchSweep(sweep).detach();
  logInfo("vcu-read", "started an in-process parameter sweep — the bike checked out as parked and out of drive");
  return {true, std::nullopt};
}

async::Task<> VcuReadRunner::watchSweep(std::shared_ptr<ParameterSweep> sweep) {
  const SweepResult result = co_await sweep->finished();
  lastComplete_ = result.snapshot.complete;
  failure_ = result.stoppedBecause;
  logInfo("vcu-read", "sweep ended (%s)", result.stoppedBecause.value_or("complete").c_str());
  lastTally_ = tallyOf(sweep->rows());
  finishedAt_ = context_.wallClockMs();
  sweep_.reset();
  if (leaseTicket_) lease_.release(*leaseTicket_);
  leaseTicket_.reset();
  stopGateWatchdog();
}

bool VcuReadRunner::cancel() {
  if (!sweep_) return false;
  sweep_->abort("stopped from the dashboard — everything read so far was kept");
  logInfo("vcu-read", "cancel requested");
  return true;
}

namespace {

// A probe as a one-shot read (cool-eva startProbe()).
class ProbeRead final : public OneShotRead {
 public:
  ProbeRead(VcuSend send, VcuProbeRequest request, const ParameterNames& names)
      : client_(std::make_shared<VcuKwpClient>(std::move(send))), request_(request), names_(names) {}
  async::Task<VcuProbeReading> run() {
    const std::shared_ptr<VcuKwpClient> client = client_;
    const VcuReadOutcome outcome = co_await client->probe(request_.target, request_.bank, request_.index);
    const VcuProbeReading reading = describeProbe(outcome, names_);
    client->stop();
    co_return reading;
  }
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) override {
    return client_->handleFrame(id, data, arrival);
  }
  void abort(std::string) override { client_->stop(); }

 private:
  std::shared_ptr<VcuKwpClient> client_;
  VcuProbeRequest request_;
  const ParameterNames& names_;
};

}  // namespace

template <typename Result, typename Module>
async::Task<std::pair<std::optional<Result>, std::string>> VcuReadRunner::runOneShot(
    std::string what, std::function<std::shared_ptr<Module>()> make) {
  using Answer = std::pair<std::optional<Result>, std::string>;
  if (const std::optional<std::string> refusal = checkBusFreeRefusals()) co_return Answer{std::nullopt, *refusal};
  const std::optional<obd::HoldTicket> hold = co_await holdObdPoller(context_, what);
  if (!hold) co_return Answer{std::nullopt, obd::pollerRefusalFor(what)};
  // With the poller parked: the same checks again, and the lease.
  std::optional<std::string> refusal = checkBusFreeRefusals();
  std::optional<BusLease::Ticket> ticket;
  if (!refusal) {
    ticket = lease_.acquire(what);
    if (!ticket) refusal = *lease_.heldBy() + " is using the bus — one thing at a time";
  }
  if (refusal) {
    context_.hold->release(*hold);
    co_return Answer{std::nullopt, *refusal};
  }
  const async::TimerId watchdog = startWatchdog([this](std::string reason) {
    if (oneShot_) oneShot_->abort(std::move(reason));
  });
  const std::shared_ptr<Module> module = make();
  async::Task<Result> running = module->run();
  oneShot_ = module;
  oneShotName_ = what;
  logInfo("vcu-read", "%s started — the bike checked out as safe to service", what.c_str());
  Result result = co_await running;
  async::currentLoop().clearTimeout(watchdog);
  oneShotName_.reset();
  oneShot_.reset();
  lease_.release(*ticket);
  context_.hold->release(*hold);
  co_return Answer{std::move(result), {}};
}

async::Task<ProbeOutcome> VcuReadRunner::probe(VcuProbeRequest request) {
  const std::string identity = std::string(microName(request.target)) + " bank " + std::to_string(request.bank) +
                               " index " + std::to_string(request.index);
  logInfo("vcu-probe", "reading bank %d index %d off %s", request.bank, request.index, microName(request.target));
  const std::function<std::shared_ptr<ProbeRead>()> make = [this, request] {
    return std::make_shared<ProbeRead>(context_.send, request, context_.store->names());
  };
  const std::pair<std::optional<VcuProbeReading>, std::string> answer =
      co_await runOneShot<VcuProbeReading, ProbeRead>(std::string("a probe"), make);
  const std::optional<VcuProbeReading>& reading = answer.first;
  const std::string& reason = answer.second;
  if (!reading) {
    logInfo("vcu-probe", "%s — %s", identity.c_str(), reason.c_str());
    co_return ProbeOutcome{std::nullopt, reason};
  }
  char identifier[16];
  std::snprintf(identifier, sizeof identifier, "0x%x", reading->identifier);
  logInfo("vcu-probe", "%s %s → %s", microName(request.target), identifier, readStatusName(reading->status));
  co_return ProbeOutcome{reading, {}};
}

async::Task<LifetimeOutcome> VcuReadRunner::readLifetimeStatistics() {
  const std::function<std::shared_ptr<LifetimeRead>()> make = [this] {
    return std::make_shared<LifetimeRead>(context_.send);
  };
  const std::pair<std::optional<LifetimeReadResult>, std::string> answer =
      co_await runOneShot<LifetimeReadResult, LifetimeRead>(std::string("a lifetime-statistics read"), make);
  if (answer.first) logInfo("vcu-read", "lifetime statistics — %s", describeMeasurement(answer.first->flowControl).c_str());
  co_return LifetimeOutcome{answer.first, answer.second};
}

async::Task<FreezeFrameOutcome> VcuReadRunner::readFreezeFrames() {
  const std::function<std::shared_ptr<FreezeFrameRead>()> make = [this] {
    return std::make_shared<FreezeFrameRead>(context_.send);
  };
  const std::pair<std::optional<FreezeFrameReadResult>, std::string> answer =
      co_await runOneShot<FreezeFrameReadResult, FreezeFrameRead>(std::string("a freeze-frame read"), make);
  if (answer.first) logInfo("vcu-read", "%s", describeFreezeFrameRead(*answer.first).c_str());
  co_return FreezeFrameOutcome{answer.first, answer.second};
}

bool VcuReadRunner::handleCanFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) {
  if (sweep_) return sweep_->handleFrame(id, data, arrival);
  if (oneShot_) return oneShot_->handleFrame(id, data, arrival);
  return false;
}

void VcuReadRunner::stop() {
  stopGateWatchdog();
  if (oneShot_) oneShot_->abort("the service is shutting down");
  if (sweep_) sweep_->abort("the service is shutting down — everything read so far was kept");
}

VcuReadRunner::Tally VcuReadRunner::tallyOf(const std::map<int, RowSummary>& rows) {
  Tally tally;
  for (const ReadStatus status : kReadStatuses) tally.byStatus[status] = 0;
  for (const auto& [index, row] : rows) {
    tally.total++;
    tally.byStatus[row.status]++;
    const int slot = microSlot(row.micro);
    tally.microSeen[slot] = true;
    if (row.status == ReadStatus::kRead) {
      tally.microRead[slot]++;
    } else {
      tally.microFailed[slot]++;
    }
  }
  tally.read = tally.byStatus[ReadStatus::kRead];
  return tally;
}

void VcuReadRunner::writeTallyJson(text::TextBuffer& out, const Tally& tally) {
  out.append("{\"total\":");
  out.appendInteger(tally.total);
  out.append(",\"read\":");
  out.appendInteger(tally.read);
  out.append(",\"byStatus\":{");
  bool first = true;
  for (const ReadStatus status : kReadStatuses) {
    if (!first) out.append(",");
    first = false;
    out.appendJsonString(readStatusName(status));
    out.append(":");
    out.appendInteger(tally.byStatus.at(status));
  }
  out.append("},\"micros\":[");
  first = true;
  // Sorted by name: A8 before A9.
  for (const Micro micro : {Micro::kA8, Micro::kA9}) {
    const int slot = microSlot(micro);
    if (!tally.microSeen[slot]) continue;
    if (!first) out.append(",");
    first = false;
    out.append("{\"micro\":");
    out.appendJsonString(microName(micro));
    out.append(",\"read\":");
    out.appendInteger(tally.microRead[slot]);
    out.append(",\"failed\":");
    out.appendInteger(tally.microFailed[slot]);
    out.append("}");
  }
  out.append("]}");
}

void VcuReadRunner::writeStateJson(text::TextBuffer& out) const {
  if (sweep_ && startedAt_) {
    out.append("{\"phase\":\"running\",\"startedAt\":");
    out.appendNumber(*startedAt_);
    out.append(",\"expected\":");
    out.appendInteger(sweep_->expected());
    out.append(",\"tally\":");
    writeTallyJson(out, tallyOf(sweep_->rows()));
    out.append("}");
    return;
  }
  if (!startedAt_ || !finishedAt_ || !lastTally_) {
    out.append("{\"phase\":\"idle\"}");
    return;
  }
  if (failure_) {
    out.append("{\"phase\":\"failed\",\"startedAt\":");
    out.appendNumber(*startedAt_);
    out.append(",\"finishedAt\":");
    out.appendNumber(*finishedAt_);
    out.append(",\"reason\":");
    out.appendJsonString(*failure_);
  } else {
    out.append("{\"phase\":\"finished\",\"startedAt\":");
    out.appendNumber(*startedAt_);
    out.append(",\"finishedAt\":");
    out.appendNumber(*finishedAt_);
    out.append(lastComplete_ ? ",\"complete\":true" : ",\"complete\":false");
  }
  out.append(",\"tally\":");
  writeTallyJson(out, *lastTally_);
  out.append("}");
}

}  // namespace emcool::vcu
