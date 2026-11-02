#include "emcool/vcu/write_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"
#include "emcool/vcu/charge_command.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/write_codec.hpp"
#include "emcool/vcu/write_targets.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr std::int64_t kGateWatchIntervalMs = 200;
constexpr std::size_t kRecentAuditLines = 12;
constexpr int kMaxParametersPerBatch = 8;

const ServiceGateVerdict kNotConsulted{true, {}, {}, std::nullopt};

std::string jsNum(double value) { return text::jsNumber(value); }

std::optional<int> parseNumber(const std::optional<std::string>& raw) {
  if (!raw) return std::nullopt;
  std::string text = *raw;
  // trim
  const auto notSpace = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
  text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
  if (text.empty()) return std::nullopt;
  const bool hex = (text.size() > 2 && (text[0] == '0' || (text[0] == '-' && text.size() > 3)) );
  // /^-?0x[0-9a-f]+$/i — the - is moved to the front of the digits before parseInt.
  std::size_t at = 0;
  bool negative = false;
  if (text[at] == '-') {
    negative = true;
    at++;
  }
  double value = 0;
  if (text.size() >= at + 2 && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X')) {
    std::string digits = text.substr(at + 2);
    if (digits.empty()) return std::nullopt;
    for (const char c : digits) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    value = static_cast<double>(std::strtoll(((negative ? "-" : "") + digits).c_str(), nullptr, 16));
  } else {
    // Number(text): a plain decimal integer here.
    char* end = nullptr;
    const std::string whole = text;
    const double parsed = std::strtod(whole.c_str(), &end);
    if (end != whole.c_str() + whole.size()) return std::nullopt;
    value = parsed;
  }
  (void)hex;
  if (std::floor(value) != value) return std::nullopt;
  return static_cast<int>(value);
}

void writeStampJson(text::TextBuffer& out, const ServiceStamp& stamp) {
  out.append("{\"raw\":{\"dateLow\":");
  out.appendInteger(stamp.raw.dateLow);
  out.append(",\"dateHigh\":");
  out.appendInteger(stamp.raw.dateHigh);
  out.append(",\"odometerLow\":");
  out.appendInteger(stamp.raw.odometerLow);
  out.append(",\"odometerHigh\":");
  out.appendInteger(stamp.raw.odometerHigh);
  out.append("},\"dateSeconds\":");
  out.appendNumber(stamp.dateSeconds);
  out.append(",\"dateIso\":");
  if (stamp.dateIso) {
    out.appendJsonString(*stamp.dateIso);
  } else {
    out.append("null");
  }
  out.append(",\"odometer\":");
  out.appendNumber(stamp.odometer);
  out.append(",\"implausible\":");
  if (stamp.implausible) {
    out.appendJsonString(*stamp.implausible);
  } else {
    out.append("null");
  }
  out.append("}");
}

void writeOnBikeJson(text::TextBuffer& out, const std::optional<ServiceWriteOnBike>& onBike) {
  if (!onBike) {
    out.append("null");
    return;
  }
  out.append("{\"name\":");
  out.appendJsonString(onBike->name);
  out.append(",\"value\":");
  out.appendNumber(onBike->value);
  out.append(",\"rawHex\":");
  if (onBike->rawHex) {
    out.appendJsonString(*onBike->rawHex);
  } else {
    out.append("null");
  }
  out.append("}");
}

void writeOptInt(text::TextBuffer& out, const std::optional<int>& value) {
  if (value) {
    out.appendInteger(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

const char* serviceWriteKindName(ServiceWriteRequest::Kind kind) {
  switch (kind) {
    case ServiceWriteRequest::Kind::kParameter:
      return "parameter";
    case ServiceWriteRequest::Kind::kParameters:
      return "parameters";
    case ServiceWriteRequest::Kind::kBit:
      return "bit";
    case ServiceWriteRequest::Kind::kReadServiceStamp:
      return "read-service-stamp";
    case ServiceWriteRequest::Kind::kSetServicePoint:
      return "set-service-point";
    case ServiceWriteRequest::Kind::kSyncClock:
      return "sync-clock";
    case ServiceWriteRequest::Kind::kClearDtcs:
      return "clear-dtcs";
    case ServiceWriteRequest::Kind::kChargeCurrent:
      return "charge-current";
    case ServiceWriteRequest::Kind::kChargeStop:
      return "charge-stop";
    case ServiceWriteRequest::Kind::kChargeSocLimit:
      return "charge-soc-limit";
    case ServiceWriteRequest::Kind::kChargeSocLimitRead:
      return "charge-soc-limit-read";
    case ServiceWriteRequest::Kind::kResetVcu:
      return "reset-vcu";
  }
  return "parameter";
}

ServiceActionPolicy serviceActionPolicy(ServiceWriteRequest::Kind kind) {
  switch (kind) {
    case ServiceWriteRequest::Kind::kParameter:
    case ServiceWriteRequest::Kind::kBit:
    case ServiceWriteRequest::Kind::kParameters:
      return {true, false, true};
    case ServiceWriteRequest::Kind::kReadServiceStamp:
    case ServiceWriteRequest::Kind::kSetServicePoint:
    case ServiceWriteRequest::Kind::kSyncClock:
    case ServiceWriteRequest::Kind::kClearDtcs:
      return {true, false, false};
    case ServiceWriteRequest::Kind::kChargeSocLimit:
    case ServiceWriteRequest::Kind::kChargeSocLimitRead:
      return {true, false, false};
    // ⚠️ EXEMPT FROM THE BIKE-STATE GATE, deliberately — a charging operation, and the gate's
    // charge evidence flaps with the trickle, so applying it would refuse a legitimate command
    // mid-charge. Their precondition is a live settled session, checked in the performer.
    case ServiceWriteRequest::Kind::kChargeCurrent:
    case ServiceWriteRequest::Kind::kChargeStop:
      return {false, false, false};
    case ServiceWriteRequest::Kind::kResetVcu:
      return {true, true, false};
  }
  return {true, false, false};
}

std::optional<std::string> serviceActionRefusal(const ServiceActionPolicy& policy, const ServiceGateVerdict& gate,
                                                const GateReader& read, ServiceWriteRequest::Kind kind) {
  if (policy.bikeStateGateApplies && !gate.safe) {
    std::string blockers;
    for (std::size_t at = 0; at < gate.blockers.size(); at++) {
      if (at > 0) blockers += "; ";
      blockers += gate.blockers[at];
    }
    return "the bike is not safe to service — " + blockers;
  }
  if (policy.refusedWhileCharging && chargePathIsActive(read)) {
    const std::string doing =
        kind == ServiceWriteRequest::Kind::kResetVcu ? "restart the VCU" : "send a " + std::string(serviceWriteKindName(kind));
    return "the charge path is live — do not " + doing + " mid-charge. Stop the charge or unplug first.";
  }
  return std::nullopt;
}

std::string utcMinute(double epochMs) {
  const std::string iso = isoFromEpochMs(epochMs);
  // slice(0,16) + "Z".
  return iso.substr(0, 16) + "Z";
}

WriteRequestParse parseWriteRequest(const std::function<std::optional<std::string>(std::string_view)>& get,
                                    const std::function<std::vector<std::string>(std::string_view)>& getAll,
                                    double nowMs) {
  WriteRequestParse parse;
  const std::optional<std::string> action = get("action");
  const std::string act = action.value_or("");
  const auto fail = [&](std::string reason) {
    parse.ok = false;
    parse.reason = std::move(reason);
    return parse;
  };
  if (act == "parameter") {
    const std::optional<std::string> name = get("name");
    if (!name || name->empty()) return fail("which parameter? pass name=");
    const std::optional<int> value = parseNumber(get("value"));
    if (!value) return fail("value must be a whole number, not " + get("value").value_or("(nothing)"));
    const std::optional<int> expected = parseNumber(get("expected"));
    if (!expected)
      return fail(
          "expected= must carry the value you read off the bike. Every write is a compare-and-swap: the Pi re-reads "
          "the parameter and refuses if it does not match.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kParameter;
    parse.request.name = *name;
    parse.request.value = *value;
    parse.request.expectedCurrent = *expected;
    return parse;
  }
  if (act == "bit") {
    const std::optional<std::string> name = get("name");
    const std::optional<std::string> bit = get("bit");
    if (!name || name->empty() || !bit || bit->empty()) return fail("a bit toggle needs name= and bit=");
    const std::optional<int> expected = parseNumber(get("expected"));
    if (!expected)
      return fail(
          "expected= must carry the whole config word as you read it off the bike — the new word is computed from it, "
          "so it cannot be guessed");
    const std::optional<std::string> on = get("on");
    if (on != "0" && on != "1") return fail("on= must be 0 or 1");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kBit;
    parse.request.name = *name;
    parse.request.bit = *bit;
    parse.request.on = on == "1";
    parse.request.expectedCurrent = *expected;
    return parse;
  }
  if (act == "parameters") {
    const std::vector<std::string> entries = getAll("w");
    if (entries.empty())
      return fail(
          "a batch write needs at least one w=NAME:VALUE:EXPECTED — the value and the value you read off the bike");
    for (const std::string& entry : entries) {
      std::vector<std::string> parts;
      std::size_t start = 0;
      for (std::size_t at = 0; at <= entry.size(); at++) {
        if (at == entry.size() || entry[at] == ':') {
          parts.push_back(entry.substr(start, at - start));
          start = at + 1;
        }
      }
      if (parts.size() != 3)
        return fail("each w= must be NAME:VALUE:EXPECTED (three colon-separated fields), not \"" + entry + "\"");
      std::string name = parts[0];
      const auto notSpace = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
      name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
      name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
      if (name.empty()) return fail("a w= entry has no parameter name: \"" + entry + "\"");
      const std::optional<int> value = parseNumber(parts[1]);
      if (!value) return fail(name + ": value must be a whole number, not " + (parts[1].empty() ? "(nothing)" : parts[1]));
      const std::optional<int> expected = parseNumber(parts[2]);
      if (!expected)
        return fail(name +
                    ": expected= must carry the value you read off the bike — every write in the batch is still a "
                    "compare-and-swap");
      parse.request.writes.push_back({name, static_cast<double>(*value), static_cast<double>(*expected)});
    }
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kParameters;
    return parse;
  }
  if (act == "read-service-stamp") {
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kReadServiceStamp;
    return parse;
  }
  if (act == "set-service-point") {
    if (get("confirm") != "set-service-point")
      return fail(
          "Set Service Point is irreversible — it stamps the bike's current clock and odometer as the last service, "
          "and there is no unset. Pass confirm=set-service-point.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kSetServicePoint;
    return parse;
  }
  if (act == "clear-dtcs") {
    if (get("confirm") != "clear-dtcs")
      return fail(
          "Clearing trouble codes is irreversible — this bike's stored list has been accumulating since before anyone "
          "started looking, and the freeze frame goes with it. Pass confirm=clear-dtcs.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kClearDtcs;
    return parse;
  }
  if (act == "sync-clock") {
    const std::optional<std::string> confirmed = get("confirm");
    if (!confirmed)
      return fail(
          "Pass confirm=<the UTC minute you showed, e.g. 2026-08-16T14:03Z>. The Pi checks it is still that minute, "
          "so a page left open cannot sync a stale time.");
    if (*confirmed != utcMinute(nowMs))
      return fail("You confirmed " + *confirmed + ", but this Pi now reads " + utcMinute(nowMs) +
                  ". Re-read the time and confirm again.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kSyncClock;
    return parse;
  }
  if (act == "charge-current") {
    const std::optional<int> amps = parseNumber(get("amps"));
    if (!amps) return fail("amps must be a whole number, not " + get("amps").value_or("(nothing)"));
    if (get("confirm") != "charge-current-" + std::to_string(*amps))
      return fail("Commanding the charge current actuates the bike's charging. Pass confirm=charge-current-" +
                  std::to_string(*amps) + " to confirm " + std::to_string(*amps) + " A.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kChargeCurrent;
    parse.request.amps = *amps;
    parse.request.origin = ServiceWriteRequest::ChargeOrigin::kManual;
    return parse;
  }
  if (act == "charge-stop") {
    if (get("confirm") != "charge-stop")
      return fail("Stopping the charge actuates the bike's charging. Pass confirm=charge-stop.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kChargeStop;
    return parse;
  }
  if (act == "charge-soc-limit") {
    const std::optional<int> percent = parseNumber(get("pct"));
    if (!percent) return fail("pct must be a whole number, not " + get("pct").value_or("(nothing)"));
    if (*percent < 0 || *percent > kMaxSocLimitPct)
      return fail("pct must be between 0 and " + std::to_string(kMaxSocLimitPct) + ", not " + std::to_string(*percent));
    const std::string expected =
        *percent == 0 ? "charge-soc-limit-off" : "charge-soc-limit-" + std::to_string(*percent);
    if (get("confirm") != expected)
      return fail(*percent == 0
                      ? "pct=0 removes the charge limit entirely — the bike would charge to full. Pass "
                        "confirm=charge-soc-limit-off."
                      : "Setting the charge limit changes what the bike does at the end of every charge. Pass confirm=" +
                            expected + " to confirm " + std::to_string(*percent) + " %.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kChargeSocLimit;
    parse.request.percent = *percent;
    return parse;
  }
  if (act == "charge-soc-limit-read") {
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kChargeSocLimitRead;
    return parse;
  }
  if (act == "reset-vcu") {
    if (get("confirm") != "reset-vcu")
      return fail(
          "Resetting the VCU restarts both micros and drops the bike off the bus briefly. Pass confirm=reset-vcu.");
    parse.ok = true;
    parse.request.kind = ServiceWriteRequest::Kind::kResetVcu;
    return parse;
  }
  return fail(
      "action must be one of parameter, parameters, bit, read-service-stamp, set-service-point, sync-clock, "
      "clear-dtcs, charge-current, charge-stop, charge-soc-limit, charge-soc-limit-read, reset-vcu — not " +
      (action ? *action : "(nothing)"));
}

WriteStatusRequest parseStatusRequest(const std::function<std::optional<std::string>(std::string_view)>& get) {
  WriteStatusRequest request;
  request.detailFor = get("detail");
  request.includeList = get("list") != "0";
  return request;
}

// ---- The runner -------------------------------------------------------------------------------

void VcuWriteRunner::audit(AuditRecord record) {
  appendAuditRecord(context_.directory, std::move(record), context_.version.label);
}

std::optional<std::string> VcuWriteRunner::refusalFor(ServiceWriteRequest::Kind kind) {
  const ServiceActionPolicy policy = serviceActionPolicy(kind);
  return serviceActionRefusal(policy, policy.bikeStateGateApplies ? context_.gate() : kNotConsulted,
                              context_.readSignal, kind);
}

bool VcuWriteRunner::handleCanFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
  return runningFrame_ ? runningFrame_(id, data) : false;
}

void VcuWriteRunner::stop() {
  if (abortRunning_) abortRunning_("the service is shutting down");
}

async::Task<VcuWriteRunner::Ready> VcuWriteRunner::checkPreconditions(const ServiceWriteRequest& request) {
  Ready ready;
  if (!context_.enabled) {
    ready.reason =
        "writing is switched off on this bike. Set SERVICE_WRITE_ENABLED=1 to allow it — it is off by default, unlike "
        "every other switch here.";
    co_return ready;
  }
  if (!context_.busIsActive) {
    ready.reason =
        "the bus is listen-only (OBD_ENABLED=0) — nothing can be transmitted, so a write would silently do nothing";
    co_return ready;
  }
  const ServiceActionPolicy policy = serviceActionPolicy(request.kind);
  if (const std::optional<std::string> refusal = refusalFor(request.kind)) {
    ready.reason = *refusal;
    co_return ready;
  }
  std::optional<TableTypeReport> tableType;
  if (policy.tableGateApplies) {
    tableType = context_.latestReport();
    const TableGateVerdict table = evaluateTableGate(tableType ? &*tableType : nullptr, names());
    if (!table.writesAllowed) {
      ready.reason = "the VCU's parameter table is " + std::string(tableGateStateName(table.state)) + " — " +
                     table.reason + " " + table.remedy;
      co_return ready;
    }
    std::vector<std::string> checkNames;
    if (request.kind == ServiceWriteRequest::Kind::kParameters) {
      for (const auto& write : request.writes) checkNames.push_back(write.name);
    } else {
      checkNames.push_back(request.name);
    }
    if (!checkNames.empty() && table.tableType) {
      const VcuParameterTable* bikeTable = parameterTableFor(*table.tableType);
      if (bikeTable == nullptr) {
        ready.reason =
            "the bike names table " + std::to_string(*table.tableType) + ", which this software cannot rebuild";
        co_return ready;
      }
      for (const std::string& name : checkNames) {
        const std::optional<WriteTarget> named = writeTargetNamed(names(), name);
        if (!named) continue;
        if (const std::optional<std::string> problem = writeTargetProblemIn(names(), *named, *bikeTable)) {
          ready.reason = "refusing to write " + named->name + " on a bike running table " +
                         std::to_string(*table.tableType) + " — " + *problem +
                         ". The parameter is writable; it is this bike's table that puts something else at that index.";
          co_return ready;
        }
      }
    }
  }
  const std::optional<BusLease::Ticket> ticket = lease_.acquire("a service write");
  if (!ticket) {
    ready.reason = std::string(*lease_.heldBy()) + " is using the bus — one thing at a time";
    co_return ready;
  }
  ready.ok = true;
  ready.tableType = tableType;
  ready.lease = ticket;
  co_return ready;
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::perform(ServiceWriteRequest request) {
  const Ready ready = co_await checkPreconditions(request);
  if (!ready.ok) {
    co_return ServiceWriteAnswer{false, {}, ready.reason};
  }
  const bool watch = serviceActionPolicy(request.kind).bikeStateGateApplies;
  const async::TimerId watchdog = watch ? startGateWatchdog() : async::TimerId{};
  ServiceWriteAnswer answer = co_await performOnBus(request, ready.tableType);
  if (watch) async::currentLoop().clearTimeout(watchdog);
  runningFrame_ = {};
  abortRunning_ = {};
  running_.reset();
  if (ready.lease) lease_.release(*ready.lease);
  co_return answer;
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performOnBus(const ServiceWriteRequest& request,
                                                             const std::optional<TableTypeReport>& tableType) {
  switch (request.kind) {
    case ServiceWriteRequest::Kind::kParameter:
    case ServiceWriteRequest::Kind::kBit:
      co_return co_await performParameterWrite(request, tableType);
    case ServiceWriteRequest::Kind::kParameters:
      co_return co_await performParameterWrites(request, tableType);
    case ServiceWriteRequest::Kind::kReadServiceStamp:
      co_return co_await performReadStamp();
    case ServiceWriteRequest::Kind::kSetServicePoint:
      co_return co_await performServicePoint();
    case ServiceWriteRequest::Kind::kSyncClock:
      co_return co_await performClockSync();
    case ServiceWriteRequest::Kind::kClearDtcs:
      co_return co_await performClearDtcs();
    case ServiceWriteRequest::Kind::kChargeCurrent:
      co_return co_await performChargeCurrent(request);
    case ServiceWriteRequest::Kind::kChargeStop:
      co_return co_await performChargeStop();
    case ServiceWriteRequest::Kind::kChargeSocLimit:
      co_return co_await performChargeSocLimit(request.percent);
    case ServiceWriteRequest::Kind::kChargeSocLimitRead:
      co_return co_await performChargeSocLimitRead();
    case ServiceWriteRequest::Kind::kResetVcu:
      co_return co_await performResetVcu();
  }
  co_return ServiceWriteAnswer{false, {}, "unreachable"};
}

async::TimerId VcuWriteRunner::startGateWatchdog() {
  auto fired = std::make_shared<bool>(false);
  auto noticed = std::make_shared<bool>(false);
  const std::function<void()> tick = [this, fired, noticed]() {
    if (*fired) return;
    const ServiceGateVerdict verdict = context_.gate();
    if (verdict.safe) return;
    std::string blockers;
    for (std::size_t at = 0; at < verdict.blockers.size(); at++) {
      if (at > 0) blockers += "; ";
      blockers += verdict.blockers[at];
    }
    if (!abortRunning_) {
      if (!*noticed) {
        *noticed = true;
        logWarning(kTag, "the bike stopped being safe to service with nothing in flight — %s", blockers.c_str());
      }
      return;
    }
    *fired = true;
    logWarning(kTag, "ABORTING — the bike stopped being safe to service: %s", blockers.c_str());
    abortRunning_("the bike stopped being safe to service — " + blockers);
  };
  return async::currentLoop().setInterval(kGateWatchIntervalMs, tick);
}

// ---- The simple performers --------------------------------------------------------------------

namespace {

std::string describeWriteOutcome(const ServiceWriteOutcome& outcome) {
  switch (outcome.status) {
    case ServiceWriteOutcome::Status::kWritten:
      return outcome.plan.description + " — written and read back as " + jsNum(outcome.readBack) + " (" +
             outcome.rawHex + ").";
    case ServiceWriteOutcome::Status::kReadBackMismatch:
      return "⚠️ " + outcome.plan.name + " was accepted but reads back as " + jsNum(outcome.readBack) + " (" +
             outcome.rawHex + "), not " + jsNum(outcome.plan.value) +
             ". The cell did not take it — the parameter may be recomputed from something else, or read-only despite "
             "acknowledging the write. Nothing was changed to what you asked for.";
    case ServiceWriteOutcome::Status::kStalePrecondition:
      return "Nothing was written. " + outcome.plan.name + " reads " + jsNum(outcome.actual) + " on the bike, not the " +
             jsNum(outcome.plan.previousValue) + " this page was showing — read it again before writing.";
    case ServiceWriteOutcome::Status::kRefused:
      return "The " + std::string(microName(outcome.plan.micro)) + " refused at the " +
             writeStageName(outcome.stage) + " step: " + outcome.description + ".";
    case ServiceWriteOutcome::Status::kFailed:
      return "Nothing confirmed. Failed at the " + std::string(writeStageName(outcome.stage)) + " step: " +
             outcome.reason;
  }
  return {};
}

std::optional<ServiceWriteOnBike> readingAfter(const ServiceWriteOutcome& outcome) {
  switch (outcome.status) {
    case ServiceWriteOutcome::Status::kWritten:
    case ServiceWriteOutcome::Status::kReadBackMismatch:
      return ServiceWriteOnBike{outcome.plan.name, outcome.readBack, outcome.rawHex};
    case ServiceWriteOutcome::Status::kStalePrecondition:
      return ServiceWriteOnBike{outcome.plan.name, outcome.actual, std::nullopt};
    case ServiceWriteOutcome::Status::kRefused:
    case ServiceWriteOutcome::Status::kFailed:
      return std::nullopt;
  }
  return std::nullopt;
}

const char* writeStatusWord(ServiceWriteOutcome::Status status) {
  switch (status) {
    case ServiceWriteOutcome::Status::kWritten:
      return "written";
    case ServiceWriteOutcome::Status::kReadBackMismatch:
      return "read-back-mismatch";
    case ServiceWriteOutcome::Status::kStalePrecondition:
      return "stale-precondition";
    case ServiceWriteOutcome::Status::kRefused:
      return "refused";
    case ServiceWriteOutcome::Status::kFailed:
      return "failed";
  }
  return "failed";
}

AuditValue beforeValue(const ServiceWriteOutcome& outcome) {
  return outcome.status == ServiceWriteOutcome::Status::kStalePrecondition ? AuditValue::of(outcome.actual)
                                                                           : AuditValue::of(outcome.plan.previousValue);
}
AuditValue afterValue(const ServiceWriteOutcome& outcome) {
  return (outcome.status == ServiceWriteOutcome::Status::kWritten ||
          outcome.status == ServiceWriteOutcome::Status::kReadBackMismatch)
             ? AuditValue::of(outcome.readBack)
             : AuditValue::null();
}
std::optional<std::string> rawHexOf(const ServiceWriteOutcome& outcome) {
  if (outcome.status == ServiceWriteOutcome::Status::kWritten ||
      outcome.status == ServiceWriteOutcome::Status::kReadBackMismatch) {
    return outcome.rawHex;
  }
  return std::nullopt;
}

}  // namespace

async::Task<ServiceWriteAnswer> VcuWriteRunner::performParameterWrite(const ServiceWriteRequest& request,
                                                                      const std::optional<TableTypeReport>& tableType) {
  const WritePlanOutcome planned =
      request.kind == ServiceWriteRequest::Kind::kParameter
          ? planWrite(names(), request.name, request.value, request.expectedCurrent)
          : planBitWrite(names(), request.name, request.bit, request.on, request.expectedCurrent);
  if (!planned.ok) co_return ServiceWriteAnswer{false, {}, planned.reason};
  const ParameterWritePlan plan = planned.plan;
  char ident[16];
  std::snprintf(ident, sizeof ident, "0x%x", plan.identifier);
  logWarning(kTag, "about to write %s (%s identifier %s)", plan.description.c_str(), microName(plan.micro), ident);

  auto session = std::make_shared<WriteSession>(context_.send, names(), cooldown_);
  running_ = session;
  runningFrame_ = [session](std::uint32_t id, std::span<const std::uint8_t> data) { return session->handleFrame(id, data); };
  abortRunning_ = [session](std::string reason) { session->abort(std::move(reason)); };
  const ServiceWriteOutcome outcome =
      co_await session->parameterWrite(plan, tableType ? &*tableType : nullptr, tableType.has_value());

  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kParameterWrite;
  record.status = writeStatusWord(outcome.status);
  record.name = plan.name;
  record.identifier = plan.identifier;
  record.micro = microName(plan.micro);
  record.before = beforeValue(outcome);
  record.after = afterValue(outcome);
  record.requested = AuditValue::of(plan.value);
  record.rawHex = rawHexOf(outcome);
  record.note = describeWriteOutcome(outcome);
  audit(std::move(record));

  ServiceWriteResult result;
  result.action = AuditAction::kParameterWrite;
  result.status = writeStatusWord(outcome.status);
  result.message = describeWriteOutcome(outcome);
  result.succeeded = outcome.status == ServiceWriteOutcome::Status::kWritten;
  result.onBikePresent = true;
  result.onBike = readingAfter(outcome);
  co_return ServiceWriteAnswer{true, result, {}};
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performParameterWrites(
    const ServiceWriteRequest& request, const std::optional<TableTypeReport>& tableType) {
  if (request.writes.empty()) co_return ServiceWriteAnswer{false, {}, "a batch write named no parameters"};
  if (static_cast<int>(request.writes.size()) > kMaxParametersPerBatch)
    co_return ServiceWriteAnswer{false, {},
                                 "a batch may write at most " + std::to_string(kMaxParametersPerBatch) +
                                     " parameters in one session; " + std::to_string(request.writes.size()) +
                                     " were asked for"};
  std::vector<ParameterWritePlan> plans;
  for (const auto& write : request.writes) {
    const WritePlanOutcome planned = planWrite(names(), write.name, write.value, write.expectedCurrent);
    if (!planned.ok) co_return ServiceWriteAnswer{false, {}, write.name + ": " + planned.reason};
    plans.push_back(planned.plan);
  }
  const Micro micro = plans.front().micro;
  for (const ParameterWritePlan& plan : plans) {
    if (plan.micro != micro) {
      co_return ServiceWriteAnswer{false, {}, "a single-session batch must be one micro; these span multiple"};
    }
  }
  auto session = std::make_shared<WriteSession>(context_.send, names(), cooldown_);
  running_ = session;
  runningFrame_ = [session](std::uint32_t id, std::span<const std::uint8_t> data) { return session->handleFrame(id, data); };
  abortRunning_ = [session](std::string reason) { session->abort(std::move(reason)); };
  const BatchWriteOutcome outcome =
      co_await session->parameterWrites(plans, tableType ? &*tableType : nullptr, tableType.has_value());

  if (outcome.status == BatchWriteOutcome::Status::kFailed) {
    AuditRecord record;
    record.at = context_.wallClockMs();
    record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
    record.action = AuditAction::kParameterWrite;
    record.status = "failed";
    record.micro = microName(micro);
    std::string names;
    for (std::size_t at = 0; at < plans.size(); at++) {
      if (at > 0) names += ", ";
      names += plans[at].name;
    }
    record.note = "batch of " + std::to_string(plans.size()) + " (" + names + ") failed at the " +
                  writeStageName(outcome.stage) + " step: " + outcome.reason;
    audit(std::move(record));
    ServiceWriteResult result;
    result.action = AuditAction::kParameterWrite;
    result.status = "failed";
    result.message =
        "Nothing was written — the batch failed at the " + std::string(writeStageName(outcome.stage)) + " step: " +
        outcome.reason;
    result.succeeded = false;
    result.hasWrites = true;
    for (const ParameterWritePlan& plan : plans) {
      result.writes.push_back({plan.name, "failed", "Not attempted — " + outcome.reason, false, std::nullopt});
    }
    co_return ServiceWriteAnswer{true, result, {}};
  }

  for (const ServiceWriteOutcome& perWrite : outcome.results) {
    AuditRecord record;
    record.at = context_.wallClockMs();
    record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
    record.action = AuditAction::kParameterWrite;
    record.status = writeStatusWord(perWrite.status);
    record.name = perWrite.plan.name;
    record.identifier = perWrite.plan.identifier;
    record.micro = microName(perWrite.plan.micro);
    record.before = beforeValue(perWrite);
    record.after = afterValue(perWrite);
    record.requested = AuditValue::of(perWrite.plan.value);
    record.rawHex = rawHexOf(perWrite);
    record.note = describeWriteOutcome(perWrite);
    audit(std::move(record));
  }

  ServiceWriteResult result;
  result.action = AuditAction::kParameterWrite;
  result.hasWrites = true;
  int writtenCount = 0;
  for (const ServiceWriteOutcome& perWrite : outcome.results) {
    const bool succeeded = perWrite.status == ServiceWriteOutcome::Status::kWritten;
    if (succeeded) writtenCount++;
    result.writes.push_back(
        {perWrite.plan.name, writeStatusWord(perWrite.status), describeWriteOutcome(perWrite), succeeded,
         readingAfter(perWrite)});
  }
  const bool allWritten = writtenCount == static_cast<int>(result.writes.size());
  result.status = allWritten ? "written" : (writtenCount > 0 ? "partial" : "failed");
  result.message = "Wrote " + std::to_string(writtenCount) + " of " + std::to_string(result.writes.size()) +
                   " parameters in one authenticated session.";
  result.succeeded = allWritten;
  co_return ServiceWriteAnswer{true, result, {}};
}

namespace {
std::string describeStampWords(const ServiceStamp& stamp) {
  // "13E8=0000 13E9=0000 13EA=0000 13EB=0000" — each identifier beside the word it answered.
  const int words[] = {stamp.raw.dateLow, stamp.raw.dateHigh, stamp.raw.odometerLow, stamp.raw.odometerHigh};
  std::string out;
  std::size_t at = 0;
  for (const ServiceStampIdentifier& entry : serviceStampIdentifiers()) {
    char text[24];
    std::snprintf(text, sizeof text, "%s%X=%04X", out.empty() ? "" : " ", entry.identifier, words[at++] & 0xffff);
    out += text;
  }
  return out;
}
std::string describeStamp(const ServiceStamp& stamp) {
  const std::string base = "Last service stamped " + (stamp.dateIso ? *stamp.dateIso : "(undecodable)") + " at " +
                           jsNum(stamp.odometer) + " (km or miles, per market).";
  return stamp.implausible ? base + " ⚠️ " + *stamp.implausible : base;
}
}  // namespace

async::Task<ServiceWriteAnswer> VcuWriteRunner::performReadStamp() {
  auto session = std::make_shared<WriteSession>(context_.send, names(), cooldown_);
  running_ = session;
  runningFrame_ = [session](std::uint32_t id, std::span<const std::uint8_t> data) { return session->handleFrame(id, data); };
  abortRunning_ = [session](std::string reason) { session->abort(std::move(reason)); };
  const StampRead outcome = co_await session->readServiceStamp(context_.wallClockMs());

  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kReadServiceStamp;
  record.status = outcome.ok ? "read" : "failed";
  record.before = outcome.ok ? (outcome.stamp.dateIso ? AuditValue::of(*outcome.stamp.dateIso) : AuditValue::null())
                             : AuditValue::null();
  record.after = outcome.ok ? AuditValue::of(outcome.stamp.odometer) : AuditValue::null();
  if (outcome.ok) record.rawHex = describeStampWords(outcome.stamp);
  record.note = outcome.ok ? outcome.stamp.implausible.value_or("read cleanly") : outcome.reason;
  audit(std::move(record));

  if (!outcome.ok) co_return ServiceWriteAnswer{false, {}, outcome.reason};
  ServiceWriteResult result;
  result.action = AuditAction::kReadServiceStamp;
  result.status = "read";
  result.message = describeStamp(outcome.stamp);
  result.succeeded = true;
  result.hasStamp = true;
  result.stampBefore = outcome.stamp;
  result.stampAfter = std::nullopt;
  co_return ServiceWriteAnswer{true, result, {}};
}

namespace {
std::string describeServicePoint(const ServicePointOutcome& outcome) {
  switch (outcome.status) {
    case ServicePointOutcome::Status::kStarted:
      if (!outcome.after) {
        return "31 FC was accepted, but the last-service block could not be read back, so what it stamped is unknown.";
      }
      return "Service point set. The bike stamped " + (outcome.after->dateIso ? *outcome.after->dateIso : "(undecodable)") +
             " at " + jsNum(outcome.after->odometer) +
             (outcome.before ? ", over " + (outcome.before->dateIso ? *outcome.before->dateIso : "(undecodable)") +
                                   " at " + jsNum(outcome.before->odometer) + "."
                             : ".") +
             (outcome.after->implausible ? " ⚠️ " + *outcome.after->implausible +
                                               " — the bike's own RTC is what it stamps."
                                         : "");
    case ServicePointOutcome::Status::kRefused:
      return "The A8 refused at the " + outcome.stage + " step: " + outcome.description + ".";
    case ServicePointOutcome::Status::kFailed:
      return "Outcome UNKNOWN — failed at the " + outcome.stage + " step: " + outcome.reason;
  }
  return {};
}
}  // namespace

async::Task<ServiceWriteAnswer> VcuWriteRunner::performServicePoint() {
  logWarning(kTag, "about to run 31 FC Set Service Point on A8 — this is irreversible");
  auto session = std::make_shared<WriteSession>(context_.send, names(), cooldown_);
  running_ = session;
  runningFrame_ = [session](std::uint32_t id, std::span<const std::uint8_t> data) { return session->handleFrame(id, data); };
  abortRunning_ = [session](std::string reason) { session->abort(std::move(reason)); };
  const ServicePointOutcome outcome = co_await session->setServicePoint(context_.wallClockMs());

  const char* statusWord = outcome.status == ServicePointOutcome::Status::kStarted    ? "started"
                           : outcome.status == ServicePointOutcome::Status::kRefused  ? "refused"
                                                                                      : "failed";
  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kSetServicePoint;
  record.status = statusWord;
  record.micro = "A8";
  record.before = outcome.status == ServicePointOutcome::Status::kStarted && outcome.before && outcome.before->dateIso
                      ? AuditValue::of(*outcome.before->dateIso)
                      : AuditValue::null();
  record.after = outcome.status == ServicePointOutcome::Status::kStarted && outcome.after && outcome.after->dateIso
                     ? AuditValue::of(*outcome.after->dateIso)
                     : AuditValue::null();
  record.note = describeServicePoint(outcome);
  audit(std::move(record));

  ServiceWriteResult result;
  result.action = AuditAction::kSetServicePoint;
  result.status = statusWord;
  result.message = describeServicePoint(outcome);
  result.succeeded = outcome.status == ServicePointOutcome::Status::kStarted;
  if (outcome.status == ServicePointOutcome::Status::kStarted) {
    result.hasStamp = true;
    result.stampBefore = outcome.before;
    result.stampAfter = outcome.after;
  }
  co_return ServiceWriteAnswer{true, result, {}};
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performClockSync() {
  const PiClockVerdict clock = checkPiClock(context_.clockEvidence());
  if (!clock.trustworthy) {
    std::string reasons;
    for (std::size_t at = 0; at < clock.reasons.size(); at++) {
      if (at > 0) reasons += "; ";
      reasons += clock.reasons[at];
    }
    co_return ServiceWriteAnswer{
        false, {}, "This bike's clock is not fit to copy into the bike: " + reasons + ". It reads " + clock.iso + "."};
  }
  const double when = context_.wallClockMs();
  const SyncClockOutcome outcome = syncBikeClock(context_.send, when);
  AuditRecord record;
  record.at = when;
  record.clockTrustworthy = true;
  record.action = AuditAction::kRtcSync;
  record.status = outcome.sent ? "sent" : "failed";
  record.requested = AuditValue::of(isoFromEpochMs(when));
  record.after = AuditValue::null();
  if (outcome.sent) record.rawHex = outcome.hex;
  record.note = outcome.sent
                    ? "broadcast on 0x120; there is no reply and no way to read the bike's clock back, so this is "
                      "unverified by construction"
                    : outcome.reason;
  audit(std::move(record));
  if (!outcome.sent) co_return ServiceWriteAnswer{false, {}, outcome.reason};
  ServiceWriteResult result;
  result.action = AuditAction::kRtcSync;
  result.status = "sent";
  result.message =
      "Broadcast " + isoFromEpochMs(when) + " UTC on 0x120 (" + outcome.hex +
      "). ⚠️ CHECK THE DASHBOARD NOW to see whether the bike took it. There is no reply to this frame and no "
      "documented way to read the bike's clock back, so the dash is the only confirmation that exists. Note the bike "
      "was sent UTC, so a dash showing local time will differ by your offset. Confirm it before using Set Service "
      "Point, which stamps whatever the bike's clock says.";
  result.succeeded = true;
  co_return ServiceWriteAnswer{true, result, {}};
}

namespace {
std::string describeResetOutcome(const ResetVcuOutcome& outcome) {
  switch (outcome.status) {
    case ResetVcuOutcome::Status::kReset:
      return "both VCU micros restarted (" + outcome.note + ")";
    case ResetVcuOutcome::Status::kRefused:
      return outcome.micro + " refused the reset: " + outcome.description;
    case ResetVcuOutcome::Status::kFailed:
      return outcome.micro + " failed at the " + outcome.stage + " step: " + outcome.reason +
             " — key off for 30 s and on to be sure of a clean state";
  }
  return {};
}
}  // namespace

async::Task<ServiceWriteAnswer> VcuWriteRunner::performResetVcu() {
  logWarning(kTag, "about to reset both VCU micros (ECUReset 11 02) — the bike drops off the bus briefly");
  auto session = std::make_shared<WriteSession>(context_.send, names(), cooldown_);
  running_ = session;
  runningFrame_ = [session](std::uint32_t id, std::span<const std::uint8_t> data) { return session->handleFrame(id, data); };
  abortRunning_ = [session](std::string reason) { session->abort(std::move(reason)); };
  const ResetVcuOutcome outcome = co_await session->resetVcu();

  const char* statusWord = outcome.status == ResetVcuOutcome::Status::kReset     ? "reset"
                           : outcome.status == ResetVcuOutcome::Status::kRefused ? "refused"
                                                                                 : "failed";
  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kResetVcu;
  record.status = statusWord;
  record.after = AuditValue::null();
  if (outcome.status != ResetVcuOutcome::Status::kReset) record.micro = outcome.micro;
  record.note = describeResetOutcome(outcome);
  audit(std::move(record));

  if (outcome.status != ResetVcuOutcome::Status::kReset) co_return ServiceWriteAnswer{false, {}, describeResetOutcome(outcome)};
  ServiceWriteResult result;
  result.action = AuditAction::kResetVcu;
  result.status = "reset";
  result.message =
      "Restarted both VCU micros (ECUReset 11 02, a key-cycle restart — nothing erased). " + outcome.note +
      ". ⚠️ The bike drops off the bus for a second or two while they reboot; the dash reconnects on its own. If a "
      "fault stays latched, key off for 30 s and on — a real power cycle clears what a reset leaves behind.";
  result.succeeded = true;
  co_return ServiceWriteAnswer{true, result, {}};
}

// ---- Status / result JSON ---------------------------------------------------------------------

namespace {

void writeClockJson(text::TextBuffer& out, const PiClockVerdict& clock) {
  if (clock.trustworthy) {
    out.append("{\"trustworthy\":true,\"iso\":");
    out.appendJsonString(clock.iso);
    out.append(",\"offsetFromGpsSeconds\":");
    out.appendNumber(clock.offsetFromGpsSeconds);
    out.append("}");
    return;
  }
  out.append("{\"trustworthy\":false,\"iso\":");
  out.appendJsonString(clock.iso);
  out.append(",\"reasons\":[");
  for (std::size_t at = 0; at < clock.reasons.size(); at++) {
    if (at > 0) out.append(",");
    out.appendJsonString(clock.reasons[at]);
  }
  out.append("]}");
}

void writeControlJson(text::TextBuffer& out, const WriteTarget& target) {
  if (target.control.kind == WriteControl::Kind::kNumber) {
    out.append("{\"kind\":\"number\",\"min\":");
    out.appendNumber(target.control.min);
    out.append(",\"max\":");
    out.appendNumber(target.control.max);
    out.append(",\"minLabel\":");
    out.appendJsonString(target.unit(target.control.min));
    out.append(",\"maxLabel\":");
    out.appendJsonString(target.unit(target.control.max));
    out.append("}");
    return;
  }
  out.append("{\"kind\":\"bits\",\"bits\":[");
  for (std::size_t at = 0; at < target.control.bits.size(); at++) {
    const WritableBit& bit = target.control.bits[at];
    if (at > 0) out.append(",");
    out.append("{\"key\":");
    out.appendJsonString(bit.key);
    out.append(",\"mask\":");
    out.appendInteger(bit.mask);
    out.append(",\"label\":");
    out.appendJsonString(bit.label);
    out.append(",\"caveat\":");
    out.appendJsonString(bit.caveat);
    out.append("}");
  }
  out.append("]}");
}

void writeRunningVersionJson(text::TextBuffer& out, const RunningVersion& version) {
  out.append("{\"commit\":");
  if (version.commit) {
    out.appendJsonString(*version.commit);
  } else {
    out.append("null");
  }
  out.append(version.dirty ? ",\"dirty\":true" : ",\"dirty\":false");
  out.append(version.trustworthy ? ",\"trustworthy\":true,\"label\":" : ",\"trustworthy\":false,\"label\":");
  out.appendJsonString(version.label);
  out.append("}");
}

}  // namespace

std::optional<VcuWriteRunner::SweptValue> VcuWriteRunner::sweptValueFor(const WriteTarget& target) {
  if (context_.store == nullptr) return std::nullopt;
  const VcuStore::Latest latest = context_.store->latest();
  if (latest.state != VcuStore::Latest::State::kSnapshot) return std::nullopt;
  SnapshotReader reader;
  if (!reader.open(latest.path, names())) return std::nullopt;
  while (const std::optional<VcuParameterRow> row = reader.next()) {
    if (row->index != target.index) continue;
    if (!row->name || *row->name != target.name || row->status != ReadStatus::kRead || row->widthMismatch ||
        !row->value) {
      return std::nullopt;
    }
    return SweptValue{*row->value, row->rawHex, target.unit(*row->value), reader.header().readAt,
                      reader.header().complete};
  }
  return std::nullopt;
}

void VcuWriteRunner::writeStatusJson(text::TextBuffer& out, const WriteStatusRequest& request) {
  const std::optional<TableTypeReport> report = context_.latestReport();
  // One snapshot open for the detail's swept value.
  std::optional<WriteTarget> named;
  if (request.detailFor) {
    for (const WriteTarget& target : writeTargets(names())) {
      if (target.name == *request.detailFor) {
        named = target;
        break;
      }
    }
  }
  out.append(context_.enabled ? "{\"enabled\":true" : "{\"enabled\":false");
  out.append(",\"chargeAck\":");
  std::optional<ChargeAckState> ack;
  if (context_.ackWatch != nullptr && context_.monotonicMs) ack = context_.ackWatch->state(context_.monotonicMs());
  if (!ack) {
    out.append("null");
  } else {
    out.append("{\"verdict\":");
    out.appendInteger(chargeAckCode(ack->verdict.kind));
    out.append(",\"message\":");
    out.appendJsonString(ack->message);
    out.append("}");
  }
  out.append(",\"runningVersion\":");
  writeRunningVersionJson(out, context_.version);
  out.append(",\"gate\":");
  writeVerdictJson(out, context_.gate());
  out.append(",\"tableGate\":");
  writeTableGateVerdictJson(out, evaluateTableGate(report ? &*report : nullptr, names()));
  out.append(",\"clock\":");
  writeClockJson(out, checkPiClock(context_.clockEvidence()));
  out.append(",\"targets\":");
  if (request.includeList) {
    out.append("[");
    bool first = true;
    for (const WriteTarget& target : writeTargets(names())) {
      if (!first) out.append(",");
      first = false;
      const VcuParameter* parameter = names().atIndex(target.index);
      out.append("{\"name\":");
      out.appendJsonString(target.name);
      out.append(",\"index\":");
      out.appendInteger(target.index);
      out.append(",\"micro\":");
      out.appendJsonString(parameter != nullptr ? microName(parameter->micro) : "?");
      out.append("}");
    }
    out.append("]");
  } else {
    out.append("null");
  }
  out.append(",\"detail\":");
  if (!named) {
    out.append("null");
  } else {
    const VcuParameter* parameter = names().atIndex(named->index);
    out.append("{\"name\":");
    out.appendJsonString(named->name);
    out.append(",\"index\":");
    out.appendInteger(named->index);
    out.append(",\"micro\":");
    out.appendJsonString(parameter != nullptr ? microName(parameter->micro) : "?");
    out.append(",\"purpose\":");
    out.appendJsonString(named->purpose);
    out.append(",\"warnings\":[");
    for (std::size_t at = 0; at < named->warnings.size(); at++) {
      if (at > 0) out.append(",");
      out.appendJsonString(named->warnings[at]);
    }
    out.append("],\"verify\":");
    if (named->verify) {
      out.appendJsonString(*named->verify);
    } else {
      out.append("null");
    }
    out.append(",\"onBike\":");
    const std::optional<SweptValue> swept = sweptValueFor(*named);
    if (!swept) {
      out.append("null");
    } else {
      out.append("{\"value\":");
      out.appendNumber(swept->value);
      out.append(",\"rawHex\":");
      if (swept->rawHex) {
        out.appendJsonString(*swept->rawHex);
      } else {
        out.append("null");
      }
      out.append(",\"label\":");
      out.appendJsonString(swept->label);
      out.append(",\"readAt\":");
      out.appendNumber(swept->readAt);
      out.append(swept->complete ? ",\"complete\":true}" : ",\"complete\":false}");
    }
    out.append(",\"control\":");
    writeControlJson(out, *named);
    out.append("}");
  }
  out.append(",\"recent\":[");
  const std::vector<AuditRecord> recent = recentAuditRecords(context_.directory, kRecentAuditLines);
  for (std::size_t at = 0; at < recent.size(); at++) {
    if (at > 0) out.append(",");
    writeAuditRecordJson(out, recent[at]);
  }
  out.append("],\"busHeldBy\":");
  if (const std::string* held = lease_.heldBy()) {
    out.appendJsonString(*held);
  } else {
    out.append("null");
  }
  out.append("}");
}

void VcuWriteRunner::writeResponseJson(text::TextBuffer& out, const WriteStatusRequest& request,
                                       const ServiceWriteResult* result, const std::optional<std::string>& message) {
  out.append("{\"status\":");
  writeStatusJson(out, request);
  out.append(",\"result\":");
  if (result == nullptr) {
    out.append("null");
  } else {
    out.append("{\"action\":");
    out.appendJsonString(auditActionName(result->action));
    out.append(",\"status\":");
    out.appendJsonString(result->status);
    out.append(",\"message\":");
    out.appendJsonString(result->message);
    out.append(result->succeeded ? ",\"succeeded\":true" : ",\"succeeded\":false");
    if (result->hasStamp) {
      out.append(",\"stamp\":{\"before\":");
      if (result->stampBefore) {
        writeStampJson(out, *result->stampBefore);
      } else {
        out.append("null");
      }
      out.append(",\"after\":");
      if (result->stampAfter) {
        writeStampJson(out, *result->stampAfter);
      } else {
        out.append("null");
      }
      out.append("}");
    }
    if (result->onBikePresent) {
      out.append(",\"onBike\":");
      writeOnBikeJson(out, result->onBike);
    }
    if (result->hasWrites) {
      out.append(",\"writes\":[");
      for (std::size_t at = 0; at < result->writes.size(); at++) {
        const PerWriteResult& write = result->writes[at];
        if (at > 0) out.append(",");
        out.append("{\"name\":");
        out.appendJsonString(write.name);
        out.append(",\"status\":");
        out.appendJsonString(write.status);
        out.append(",\"message\":");
        out.appendJsonString(write.message);
        out.append(write.succeeded ? ",\"succeeded\":true,\"onBike\":" : ",\"succeeded\":false,\"onBike\":");
        writeOnBikeJson(out, write.onBike);
        out.append("}");
      }
      out.append("]");
    }
    if (result->hasClear) {
      out.append(",\"clear\":{\"storedBefore\":");
      writeOptInt(out, result->clear.storedBefore);
      out.append(",\"storedAfter\":");
      writeOptInt(out, result->clear.storedAfter);
      out.append(",\"distSinceClearBeforeKm\":");
      writeOptInt(out, result->clear.distSinceClearBeforeKm);
      out.append(",\"distSinceClearAfterKm\":");
      writeOptInt(out, result->clear.distSinceClearAfterKm);
      out.append(",\"listedAfter\":");
      writeOptInt(out, result->clear.listedAfter);
      out.append(",\"verdict\":");
      out.appendJsonString(result->clear.verdict);
      out.append("}");
    }
    out.append("}");
  }
  out.append(",\"message\":");
  if (message) {
    out.appendJsonString(*message);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace emcool::vcu
