#include "emcool/vcu/service_gate.hpp"

#include <cmath>

#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr double kBroadcastMaxAgeMs = 1000;
constexpr double kPollMaxAgeMs = 10000;
constexpr double kChargerFrameMaxAgeMs = 2000;
constexpr int kInletPresentMask = 0x08;

enum class WhileCharging : std::uint8_t { kNothing, kExcuseUnsafe, kAllowAbsent };

struct Rule {
  std::string_view key;
  std::string_view requirement;
  double maxAgeMs;
  bool required;
  WhileCharging whileCharging;
};

// Every rule is "reads exactly zero".
constexpr Rule kRules[] = {
    {"speed_can_kmh", "road speed is zero", kBroadcastMaxAgeMs, true, WhileCharging::kAllowAbsent},
    {"motor_rpm_can", "the motor is not turning", kBroadcastMaxAgeMs, true, WhileCharging::kAllowAbsent},
    {"moving", "the bike is not moving", kBroadcastMaxAgeMs, true, WhileCharging::kNothing},
    {"energized", "the drive is not energized", kBroadcastMaxAgeMs, true, WhileCharging::kExcuseUnsafe},
    {"go", "the bike is not in drive", kBroadcastMaxAgeMs, true, WhileCharging::kNothing},
    {"go_request", "nobody is asking for drive", kBroadcastMaxAgeMs, true, WhileCharging::kNothing},
    {"throttle_on", "the throttle is closed", kBroadcastMaxAgeMs, true, WhileCharging::kNothing},
    {"speed_kmh", "OBD road speed agrees that speed is zero", kPollMaxAgeMs, false, WhileCharging::kNothing},
};

enum class Counts : std::uint8_t { kIsOne, kAlways, kChargeManagerSession };
struct EvidenceRule {
  std::string_view key;
  Counts counts;
  double maxAgeMs;
};
constexpr EvidenceRule kEvidence[] = {
    {"fast_dc_contactor", Counts::kIsOne, kBroadcastMaxAgeMs},
    {"dc_v", Counts::kAlways, kChargerFrameMaxAgeMs},
    {"dc_a", Counts::kAlways, kChargerFrameMaxAgeMs},
    {"mains_v", Counts::kAlways, kChargerFrameMaxAgeMs},
    {"mains_a", Counts::kAlways, kChargerFrameMaxAgeMs},
    {"charge_manager_state", Counts::kChargeManagerSession, kChargeSessionMaxAgeMs},
};

constexpr std::string_view kInletKey = "charge_manager_status";
constexpr std::string_view kInletRequirement = "the charge manager sees a cable in the inlet";
constexpr const char* kInletBlocker =
    "the charge manager reports nothing in the inlet — a cable that is not seated cannot make the bike safe to service";

constexpr std::string_view kGateKeys[] = {
    "speed_can_kmh", "motor_rpm_can", "moving",  "energized", "go",      "go_request",
    "throttle_on",   "speed_kmh",     "fast_dc_contactor", "dc_v", "dc_a", "mains_v",
    "mains_a",       "charge_manager_state", "charge_manager_status",
};

std::string meaning(const EvidenceRule& rule, double value) {
  if (rule.key == "fast_dc_contactor") return "the DC fast-charge contactor is closed";
  if (rule.key == "dc_v") return "the AC charger is reporting DC volts";
  if (rule.key == "dc_a") return "the AC charger is reporting DC amps";
  if (rule.key == "mains_v") return "the AC charger is reporting mains volts";
  if (rule.key == "mains_a") return "the AC charger is reporting mains amps";
  return value == kChargeManagerStateDc ? "the charge manager reports a live DC session"
                                        : "the charge manager reports a live AC session";
}

bool counts(const EvidenceRule& rule, double value) {
  switch (rule.counts) {
    case Counts::kIsOne:
      return value == 1;
    case Counts::kAlways:
      return true;
    case Counts::kChargeManagerSession:
      return value == kChargeManagerStateAc || value == kChargeManagerStateDc;
  }
  return false;
}

GateCheck checkRule(const Rule& rule, const GateSample* sample, bool charging) {
  GateCheck check{rule.key, rule.requirement, GateCheckState::kMissing, std::nullopt, std::nullopt, rule.required};
  if (sample != nullptr) {
    check.value = sample->value;
    check.ageMs = sample->ageMs;
  }
  const bool absenceAllowed = rule.whileCharging == WhileCharging::kAllowAbsent && charging;
  if (!check.value || !check.ageMs) {
    check.state = absenceAllowed ? GateCheckState::kExcusedByCharging : GateCheckState::kMissing;
  } else if (*check.ageMs > rule.maxAgeMs) {
    check.state = absenceAllowed ? GateCheckState::kExcusedByCharging : GateCheckState::kStale;
  } else if (*check.value == 0) {
    check.state = GateCheckState::kOk;
  } else {
    check.state = rule.whileCharging == WhileCharging::kExcuseUnsafe && charging ? GateCheckState::kExcusedByCharging
                                                                                 : GateCheckState::kUnsafe;
  }
  return check;
}

GateCheck inletCheck(const GateReadings& readings, bool inletEmpty) {
  const GateSample* sample = readings.find(kInletKey);
  GateCheck check{kInletKey, kInletRequirement, GateCheckState::kMissing, std::nullopt, std::nullopt, false};
  if (sample != nullptr) {
    check.value = sample->value;
    check.ageMs = sample->ageMs;
  }
  if (inletEmpty) {
    check.state = GateCheckState::kInletEmpty;
  } else if (!check.value || !check.ageMs) {
    check.state = GateCheckState::kMissing;
  } else if (*check.ageMs > kChargeSessionMaxAgeMs) {
    check.state = GateCheckState::kStale;
  } else {
    check.state = GateCheckState::kOk;
  }
  return check;
}

std::string formatValue(const std::optional<double>& value) {
  if (!value) return "nothing";
  return std::floor(*value) == *value ? jsNumber(*value) : jsToFixed(*value, 1);
}

std::string describeBlocker(const GateCheck& check) {
  const std::string requirement(check.requirement);
  switch (check.state) {
    case GateCheckState::kUnsafe:
      return requirement + " — it reads " + formatValue(check.value);
    case GateCheckState::kStale:
      return requirement + ": " + std::string(check.key) + " last arrived " +
             jsNumber(jsRound(check.ageMs.value_or(0) / 1000)) + " s ago, too old to go on";
    case GateCheckState::kMissing:
      return requirement + ": " + std::string(check.key) + " has never arrived, so there is nothing to check";
    case GateCheckState::kOk:
    case GateCheckState::kExcusedByCharging:
    case GateCheckState::kInletEmpty:
      break;
  }
  return requirement + " (no fault; this should not have been reported)";
}

std::vector<std::string> blockersFrom(const std::vector<GateCheck>& checks) {
  std::vector<std::string> blockers;
  for (const GateCheck& check : checks) {
    if (check.state == GateCheckState::kOk || check.state == GateCheckState::kExcusedByCharging ||
        check.state == GateCheckState::kInletEmpty) {
      continue;
    }
    if ((check.state == GateCheckState::kMissing || check.state == GateCheckState::kStale) && !check.required) continue;
    blockers.push_back(describeBlocker(check));
  }
  return blockers;
}

void writeOptionalNumber(text::TextBuffer& out, const std::optional<double>& value) {
  if (value) {
    out.appendNumber(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

std::span<const std::string_view> serviceGateSignalKeys() { return kGateKeys; }

const GateSample* GateReadings::find(std::string_view key) const {
  for (const auto& [name, sample] : samples) {
    if (name == key) return &sample;
  }
  return nullptr;
}

GateReadings sampleServiceGate(const GateReader& read) {
  GateReadings readings;
  for (const std::string_view key : kGateKeys) readings.samples.emplace_back(key, read(key));
  return readings;
}

const char* gateCheckStateName(GateCheckState state) {
  switch (state) {
    case GateCheckState::kOk:
      return "ok";
    case GateCheckState::kUnsafe:
      return "unsafe";
    case GateCheckState::kStale:
      return "stale";
    case GateCheckState::kMissing:
      return "missing";
    case GateCheckState::kExcusedByCharging:
      return "excused-by-charging";
    case GateCheckState::kInletEmpty:
      return "inlet-empty";
  }
  return "missing";
}

std::optional<std::string> findChargingEvidence(const GateReadings& readings) {
  for (const EvidenceRule& rule : kEvidence) {
    const GateSample* sample = readings.find(rule.key);
    if (sample == nullptr || !sample->value || !sample->ageMs || *sample->ageMs > rule.maxAgeMs) continue;
    if (counts(rule, *sample->value)) return meaning(rule, *sample->value);
  }
  return std::nullopt;
}

bool inletIsEmpty(const GateReadings& readings) {
  const GateSample* sample = readings.find(kInletKey);
  if (sample == nullptr || !sample->value || !sample->ageMs || *sample->ageMs > kChargeSessionMaxAgeMs) return false;
  // JavaScript's & on a number: its 32-bit integer.
  const auto bits = static_cast<std::int32_t>(static_cast<std::int64_t>(*sample->value));
  return (bits & kInletPresentMask) == 0;
}

bool chargeManagerIsLive(std::optional<double> value, std::optional<double> ageMs) {
  return value.has_value() && ageMs.has_value() && *ageMs <= kChargeSessionMaxAgeMs;
}

std::optional<ChargeSession> chargeSessionFrom(std::optional<double> value, std::optional<double> ageMs) {
  if (!value || !ageMs || *ageMs > kChargeSessionMaxAgeMs) return std::nullopt;
  if (*value == kChargeManagerStateAc) return ChargeSession{*value, false};
  if (*value == kChargeManagerStateDc) return ChargeSession{*value, true};
  return std::nullopt;
}

bool chargePathIsActive(const GateReader& read) {
  GateReadings readings;
  for (const EvidenceRule& rule : kEvidence) readings.samples.emplace_back(rule.key, read(rule.key));
  readings.samples.emplace_back(kInletKey, read(kInletKey));
  if (findChargingEvidence(readings).has_value()) return true;
  const GateSample* state = readings.find("charge_manager_state");
  if (state == nullptr) return false;
  return chargeManagerIsLive(state->value, state->ageMs);
}

bool verdictSeesACable(const ServiceGateVerdict& verdict) {
  if (verdict.chargingEvidence) return true;
  for (const GateCheck& check : verdict.checks) {
    if (check.key == kInletKey) return check.state == GateCheckState::kOk;
  }
  return false;
}

ServiceGateVerdict evaluateServiceGate(const GateReadings& readings) {
  const bool inletEmpty = inletIsEmpty(readings);
  const std::optional<std::string> witnessed = findChargingEvidence(readings);
  ServiceGateVerdict verdict;
  verdict.chargingEvidence = inletEmpty ? std::nullopt : witnessed;
  for (const Rule& rule : kRules) {
    verdict.checks.push_back(checkRule(rule, readings.find(rule.key), verdict.chargingEvidence.has_value()));
  }
  verdict.checks.push_back(inletCheck(readings, inletEmpty));
  verdict.blockers = blockersFrom(verdict.checks);
  // Name the veto when it's the reason: had the charge evidence been believed, the gate would pass.
  if (inletEmpty && witnessed && !verdict.blockers.empty()) {
    std::vector<GateCheck> believed;
    for (const Rule& rule : kRules) believed.push_back(checkRule(rule, readings.find(rule.key), true));
    if (blockersFrom(believed).empty()) verdict.blockers.insert(verdict.blockers.begin(), kInletBlocker);
  }
  verdict.safe = verdict.blockers.empty();
  return verdict;
}

std::vector<std::string> restartMotionBlockers(const GateReadings& readings) {
  std::vector<std::string> blockers;
  for (const Rule& rule : kRules) {
    if (rule.whileCharging == WhileCharging::kExcuseUnsafe) continue;
    const GateSample* sample = readings.find(rule.key);
    if (sample == nullptr || !sample->value || !sample->ageMs || *sample->ageMs > rule.maxAgeMs) continue;
    if (*sample->value != 0) blockers.emplace_back(rule.requirement);
  }
  return blockers;
}

void writeVerdictJson(text::TextBuffer& out, const ServiceGateVerdict& verdict) {
  out.append(verdict.safe ? "{\"safe\":true,\"blockers\":[" : "{\"safe\":false,\"blockers\":[");
  for (std::size_t at = 0; at < verdict.blockers.size(); at++) {
    if (at > 0) out.append(",");
    out.appendJsonString(verdict.blockers[at]);
  }
  out.append("],\"checks\":[");
  for (std::size_t at = 0; at < verdict.checks.size(); at++) {
    const GateCheck& check = verdict.checks[at];
    if (at > 0) out.append(",");
    out.append("{\"key\":");
    out.appendJsonString(check.key);
    out.append(",\"requirement\":");
    out.appendJsonString(check.requirement);
    out.append(",\"value\":");
    writeOptionalNumber(out, check.value);
    out.append(",\"ageMs\":");
    writeOptionalNumber(out, check.ageMs);
    out.append(check.required ? ",\"required\":true" : ",\"required\":false");
    out.append(",\"state\":");
    out.appendJsonString(gateCheckStateName(check.state));
    out.append("}");
  }
  out.append("],\"chargingEvidence\":");
  if (verdict.chargingEvidence) {
    out.appendJsonString(*verdict.chargingEvidence);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace emcool::vcu
