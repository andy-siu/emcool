#include "emcool/vcu/write_targets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "emcool/text/js.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/table_gate.hpp"

namespace emcool::vcu {

namespace {

std::string upper(std::string_view text) {
  std::string out(text);
  for (char& character : out) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  return out;
}

std::string hexWord(double raw) {
  const long long value = static_cast<long long>(raw);
  char text[24];
  std::snprintf(text, sizeof text, "%llX", value < 0 ? -value : value);
  std::string digits = value < 0 ? "-" + std::string(text) : text;
  // padStart(4, "0") on the digit text (sign not counted the way JS pads the whole string; VSM is
  // unsigned, so this only ever pads a non-negative value).
  while (digits.size() < 4) digits = "0" + digits;
  return digits;
}

int identifierForIndex(int index) { return (1 << 12) | index; }

std::vector<std::uint8_t> encodeRecordOrEmpty(double value, int width, bool isSigned, bool& ok) {
  const int bits = width * 8;
  const double low = isSigned ? -std::ldexp(1.0, bits - 1) : 0;
  const double high = isSigned ? std::ldexp(1.0, bits - 1) - 1 : std::ldexp(1.0, bits) - 1;
  if (std::floor(value) != value || value < low || value > high) {
    ok = false;
    return {};
  }
  const std::uint64_t unsignedValue =
      value < 0 ? static_cast<std::uint64_t>(value + std::ldexp(1.0, bits)) : static_cast<std::uint64_t>(value);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width));
  for (int position = width - 1; position >= 0; position--) {
    bytes[static_cast<std::size_t>(position)] =
        static_cast<std::uint8_t>((unsignedValue >> ((width - 1 - position) * 8)) & 0xff);
  }
  ok = true;
  return bytes;
}

// The active-table parameter for a target, with the self-consistency check. problem non-empty = refuse.
struct ParameterFor {
  const VcuParameter* parameter = nullptr;
  std::string problem;
};
ParameterFor parameterFor(const ParameterNames& names, const WriteTarget& target) {
  const VcuParameter* parameter = names.atIndex(target.index);
  if (parameter == nullptr) {
    return {nullptr, "vcu-write: the allowlist names index " + std::to_string(target.index) +
                         ", which params.ecf does not describe"};
  }
  if (upper(parameter->name()) != upper(target.name)) {
    return {nullptr, "vcu-write: the allowlist calls index " + std::to_string(target.index) + " \"" + target.name +
                         "\", params.ecf calls it \"" + std::string(parameter->name()) +
                         "\" — one of them is wrong, and writing to the wrong calibration cell is not a risk worth "
                         "taking"};
  }
  return {parameter, {}};
}

WritePlanOutcome buildPlan(const ParameterNames& names, const WriteTarget& target, double value, double previousValue) {
  const ParameterFor found = parameterFor(names, target);
  if (found.parameter == nullptr) return {false, {}, found.problem};
  const int width = recordLengthFor(found.parameter->type);
  bool ok = false;
  const std::vector<std::uint8_t> encoded = encodeRecordOrEmpty(value, width, found.parameter->isSigned, ok);
  if (!ok) {
    return {false, {},
            target.name + " is a " + std::to_string(width) + "-byte " +
                (found.parameter->isSigned ? "signed" : "unsigned") + " record and cannot hold " +
                text::jsNumber(value)};
  }
  ParameterWritePlan plan;
  plan.name = target.name;
  plan.index = target.index;
  plan.micro = found.parameter->micro;
  plan.identifier = identifierForIndex(target.index);
  plan.record = encoded;
  plan.value = value;
  plan.previousValue = previousValue;
  plan.description = target.name + ": " + target.unit(previousValue) + " → " + target.unit(value);
  return {true, plan, {}};
}

std::string refusalFor(const ParameterNames& names, std::string_view name) {
  const std::string wanted = upper(name);
  std::vector<int> duplicates;
  for (const VcuParameter& parameter : names.active().parameters) {
    if (upper(parameter.name()) == wanted) duplicates.push_back(parameter.index);
  }
  // cool-eva quotes the name with curly quotes “ ” (U+201C/U+201D), not straight ASCII.
  const std::string open = "“";
  const std::string close = "”";
  if (duplicates.size() > 1) {
    std::string indices;
    for (std::size_t at = 0; at < duplicates.size(); at++) {
      if (at > 0) indices += ", ";
      indices += std::to_string(duplicates[at]);
    }
    return open + std::string(name) + close + " names " + std::to_string(duplicates.size()) +
           " different parameters in this table (indices " + indices +
           "), so a write cannot say which. Nothing here addresses a parameter by index, on purpose.";
  }
  return open + std::string(name) + close + " is not a parameter in this bike's table. The full table is at /params.html.";
}

WriteControl datatypeControl(const VcuParameter& parameter) {
  WriteControl control;
  control.kind = WriteControl::Kind::kNumber;
  if (parameter.type == StorageType::kBool) {
    control.min = 0;
    control.max = 1;
    return control;
  }
  const int bits = recordLengthFor(parameter.type) * 8;
  if (parameter.isSigned) {
    control.min = -std::ldexp(1.0, bits - 1);
    control.max = std::ldexp(1.0, bits - 1) - 1;
  } else {
    control.min = 0;
    control.max = std::ldexp(1.0, bits) - 1;
  }
  return control;
}

WriteTarget generatedTarget(const VcuParameter& parameter) {
  WriteTarget target;
  target.name = parameter.name();
  target.index = parameter.index;
  target.control = datatypeControl(parameter);
  target.unitKind = UnitKind::kPlain;
  const std::string storage = std::string(storageTypeName(parameter.type)) +
                              (parameter.isSigned ? " signed" : " unsigned") + " on the " + microName(parameter.micro);
  const std::string section = parameter.section() != nullptr ? std::string(parameter.section()) : "";
  target.purpose = "Parameter " + std::to_string(parameter.index) + ", " + storage +
                   (section.empty() ? "" : ", under \"" + section + "\"") +
                   ". Nothing in this repo knows what it does or what a sane value is — the name is the whole of what "
                   "is known.";
  target.warnings = {
      "⚠️ NOT researched. This is reachable because every parameter is, not because anyone established it is safe to "
      "change.",
      "⚠️ The range " + text::jsNumber(target.control.min) + "…" + text::jsNumber(target.control.max) +
          " is the datatype's full range, not a safe range. A value inside it can still be one the bike cannot run on.",
      "⚠️ Read the current value first and write it down. There is no factory default in this repo to put back.",
      "A dealer visit reverts it. The service tool reinstalls parameter values from Energica's server, keyed by VIN.",
  };
  target.verify = std::nullopt;
  return target;
}

WritePlanOutcome rebuildBitPlan(const ParameterNames& names, const WriteTarget& target, double value,
                                double previousValue) {
  if (target.control.kind != WriteControl::Kind::kBits) {
    return {false, {}, target.name + " is not a bit field"};
  }
  if (std::floor(value) != value || value < 0 || value > 0xffff) {
    return {false, {}, text::jsNumber(value) + " is not a 16-bit word"};
  }
  const int changed = (static_cast<int>(value) ^ static_cast<int>(previousValue)) & 0xffff;
  const WritableBit* bit = nullptr;
  for (const WritableBit& candidate : target.control.bits) {
    if (changed == candidate.mask || changed == 0) {
      bit = &candidate;
      break;
    }
  }
  if (bit == nullptr) {
    char maskHex[16];
    std::snprintf(maskHex, sizeof maskHex, "0x%x", changed);
    return {false, {},
            "writing " + text::jsNumber(value) + " over " + text::jsNumber(previousValue) + " would change mask " +
                maskHex + ", which no writable bit of " + target.name + " accounts for"};
  }
  const bool on = (static_cast<int>(value) & bit->mask) != 0;
  return planBitWrite(names, target.name, bit->key, on, previousValue);
}

}  // namespace

std::string WriteTarget::unit(double raw) const {
  switch (unitKind) {
    case UnitKind::kAmps:
      return text::jsNumber(raw) + " A";
    case UnitKind::kRawUnknown:
      return text::jsNumber(raw) + " (raw — the unit is not known)";
    case UnitKind::kDeciNm:
      return text::jsToFixed(raw / 10, 1) + " Nm";
    case UnitKind::kHexWord:
      return "0x" + hexWord(raw);
    case UnitKind::kPlain:
      return text::jsNumber(raw);
  }
  return text::jsNumber(raw);
}

std::vector<WriteTarget> curatedWriteTargets() {
  std::vector<WriteTarget> targets;

  WriteTarget dc;
  dc.name = "MAX_DC_CHG_CURRENT";
  dc.index = 258;
  dc.control = {WriteControl::Kind::kNumber, 0, 80, {}};
  dc.unitKind = UnitKind::kAmps;
  dc.purpose =
      "The DC fast-charge current this bike advertises to a charger, in amperes. Reads 75. Energica's own 60/75/80 A "
      "options write this parameter and nothing else, so 80 is a value the factory shipped.";
  dc.warnings = {
      "⚠️ It will probably do nothing. Across eight logged DC sessions the ceiling is the STATION, not the bike: "
      "station identity explains 84 % of the variance, the highest ever delivered is 73.2 A, and no station has "
      "offered even the 75 A already permitted.",
      "⚠️ 80 A is 1.25C for this pack. The cell datasheet allows 1.10C = 70.4 A, and only between 25 and 35 °C — and "
      "the VCU is shown 35 °C while the pack is really at 44-54 °C, so 91 % of DC charging time above 30 A is already "
      "over the cell's fast-charge ceiling. This raises the ceiling on an exposure that is already there.",
      "A dealer visit reverts it. The service tool reinstalls parameter values from Energica's server, keyed by VIN.",
  };
  dc.verify =
      "Free check, no charger needed: 0x625 b2 is the configured max DC current and is broadcast on a merely-awake "
      "bike. It should now read the value you wrote (0x50 for 80 A, 0x4B for 75). If it does not, the write did not "
      "take.";
  targets.push_back(std::move(dc));

  WriteTarget gain;
  gain.name = "FCHG_CURRENT_GAIN";
  gain.index = 259;
  gain.control = {WriteControl::Kind::kNumber, 0, 512, {}};
  gain.unitKind = UnitKind::kRawUnknown;
  gain.purpose =
      "An EVSE-block scalar whose meaning has never been established. Reads 225. Absent from Energica's tooling "
      "entirely — no option, no label, no range, no code path.";
  gain.warnings = {
      "⚠️⚠️ THE DIRECTION OF EFFECT IS UNKNOWN, and this is a genuine 50/50 rather than a gap someone forgot to close. "
      "If it is a MEASUREMENT CALIBRATION — which the name argues for — then raising it makes the bike believe it is "
      "drawing ~13 % more than it is, and it would back off SOONER, not later. If it sits in a denominator instead, "
      "it would draw more. Nothing anywhere derives which.",
      "⚠️ Third possibility, and it is not remote: the DC charge is regulated by a PID loop (CELLV_KA/KAI/KAD against "
      "CELL_TARGET_DC), and a “gain” of 225 in the EVSE block may be a controller gain in that loop. If so, 225 → 255 "
      "changes loop DYNAMICS, not the ceiling — and a badly chosen loop gain oscillates.",
      "⚠️ The arithmetic that produced 255 has been RETRACTED. “75 × 225/255 = 66.18 A” matched an observed ceiling "
      "to three figures, which is where 255 came from — but the wire request was later measured at 75 while 66.2 A "
      "flowed, and 73.2 A was delivered on another day with no parameter change. Both facts are incompatible with a "
      "fixed 88 % gate on the request. It is now treated as numerology.",
      "Change ONE parameter per charge session. With the station explaining 84 % of the variance, two changes at once "
      "cannot be told apart afterwards. Get MAX_DC_CHG_CURRENT = 80 into the bag first.",
  };
  gain.verify =
      "No broadcast field carries this one, so the read-back above is the only confirmation the cell took it. What it "
      "DOES is a question for the next DC session — logged, at a station you have used before, with nothing else "
      "changed.";
  targets.push_back(std::move(gain));

  WriteTarget torque;
  torque.name = "TORQUE_LIMIT";
  torque.index = 48;
  torque.control = {WriteControl::Kind::kNumber, 0, 2760, {}};
  torque.unitKind = UnitKind::kDeciNm;
  torque.purpose = "The drive torque ceiling, 0.1 Nm per count. Reads 230.0 Nm.";
  torque.warnings = {
      "⚠️ Raising this raises what the bike will deliver at full throttle, which is a change to how the motorcycle "
      "behaves under your hand. Not a bench setting.",
      "Torque may already be clipping against ACTIVE_CURRENT_LIMIT (400 A), which is NOT on this allowlist. If nothing "
      "changes, that is the likely reason.",
      "The 276.0 Nm ceiling is this repo's policy (+20 % on today's value), not a measured hardware limit.",
  };
  torque.verify =
      "Nothing broadcasts this, so the read-back above is the only confirmation the cell took it — a parameter sweep "
      "re-reads it later if you want it in the record. Whether it changed anything is felt at full throttle, and "
      "ACTIVE_CURRENT_LIMIT may cap it first.";
  targets.push_back(std::move(torque));

  WriteTarget regen;
  regen.name = "REGEN_TORQUE_LIMIT";
  regen.index = 49;
  regen.control = {WriteControl::Kind::kNumber, 0, 900, {}};
  regen.unitKind = UnitKind::kDeciNm;
  regen.purpose =
      "The regenerative braking torque ceiling, 0.1 Nm per count. Reads 60.0 Nm (raised from the factory 50.0 on "
      "2026-08-09).";
  regen.warnings = {
      "⚠️ REGEN_MAP0..3_TRQ (63-66) read 10/20/30/40 and are almost certainly PERCENTAGES of this limit, so raising "
      "this scales every regen map at once.",
      "Engine braking that suddenly got stronger is a handling change, felt first when you close the throttle "
      "mid-corner.",
      "It may not change anything: REGEN_CURRENT_LIMIT (142) = 120 A is unchanged and is the suspected real cap.",
  };
  regen.verify =
      "Nothing broadcasts this either — the read-back above is the confirmation that the cell took it. Whether it does "
      "anything shows up as regen current on the next ride: if that still tops out around 120 A, REGEN_CURRENT_LIMIT "
      "was the cap all along.";
  targets.push_back(std::move(regen));

  WriteTarget vsm;
  vsm.name = "VSM_CONFIG_1";
  vsm.index = 16;
  vsm.control.kind = WriteControl::Kind::kBits;
  vsm.control.bits = {{"heated-handlebars", 0x0004, "Heated handlebars",
                       "Energica's option OP0024 sets exactly this bit and nothing else — but activation is normally "
                       "granted per-VIN on Energica's server, and the option carries a firmware floor (FW.Min.V "
                       "*.042). The bit may therefore write and read back correctly and the feature still not appear, "
                       "because the grip logic lives in the DASHBOARD's firmware, not the VCU's."}};
  vsm.unitKind = UnitKind::kHexWord;
  vsm.purpose =
      "The VCU's option word. Only the heated-handlebar bit is offered here; the same word also carries the PSU type "
      "and the Bluetooth variant.";
  vsm.warnings = {
      "⚠️ The whole word is NOT writable through this repo, on purpose. A raw word write here would reconfigure the "
      "PSU type (mask 0x0760) or Bluetooth (mask 0x3000) — the failure mode another owner's tool warns about by name.",
      "⚠️ A successful write does NOT mean the feature turned on. See the bit's own note.",
      "This changes a flag, not wiring. There is no heated-grip circuit on this bike unless one was fitted.",
  };
  vsm.verify =
      "⚠️ Config words are only read at BOOT: key-cycle the bike before judging it. The read-back above proves the "
      "word in the VCU's EEPROM changed; it says nothing about whether the dashboard offers the grips, which is a "
      "separate firmware's decision.";
  targets.push_back(std::move(vsm));

  return targets;
}

std::vector<WriteTarget> writeTargets(const ParameterNames& names) {
  const std::vector<WriteTarget> curated = curatedWriteTargets();
  std::set<std::string> curatedNames;
  for (const WriteTarget& target : curated) curatedNames.insert(upper(target.name));
  std::map<std::string, int> seen;
  for (const VcuParameter& parameter : names.active().parameters) seen[upper(parameter.name())]++;
  std::vector<WriteTarget> targets = curated;
  for (const VcuParameter& parameter : names.active().parameters) {
    const std::string key = upper(parameter.name());
    if (curatedNames.count(key) != 0) continue;
    if (seen[key] > 1) continue;  // duplicated names are unaddressable
    targets.push_back(generatedTarget(parameter));
  }
  return targets;
}

std::vector<std::string> writeTargetNames(const ParameterNames& names) {
  std::vector<std::string> out;
  for (const WriteTarget& target : writeTargets(names)) out.push_back(target.name);
  return out;
}

std::optional<WriteTarget> writeTargetNamed(const ParameterNames& names, std::string_view name) {
  const std::string wanted = upper(name);
  for (const WriteTarget& target : writeTargets(names)) {
    if (upper(target.name) == wanted) return target;
  }
  return std::nullopt;
}

WritePlanOutcome planWrite(const ParameterNames& names, std::string_view name, double requestedValue,
                           double previousValue) {
  const std::optional<WriteTarget> target = writeTargetNamed(names, name);
  if (!target) return {false, {}, refusalFor(names, name)};
  if (target->control.kind != WriteControl::Kind::kNumber) {
    return {false, {},
            target->name +
                " is a config word — its bits are toggled by name, and the word itself is deliberately not writable"};
  }
  if (std::floor(requestedValue) != requestedValue) {
    return {false, {}, target->name + " takes a whole number, not " + text::jsNumber(requestedValue)};
  }
  if (requestedValue < target->control.min || requestedValue > target->control.max) {
    return {false, {},
            target->name + " may be set to " + text::jsNumber(target->control.min) + "…" +
                text::jsNumber(target->control.max) + " (" + target->unit(target->control.min) + "…" +
                target->unit(target->control.max) + "), not " + text::jsNumber(requestedValue) +
                ". That bound is this repo's policy, written down with its reasoning in write_targets."};
  }
  return buildPlan(names, *target, requestedValue, previousValue);
}

WritePlanOutcome planBitWrite(const ParameterNames& names, std::string_view name, std::string_view bitKey, bool on,
                              double currentValue) {
  const std::optional<WriteTarget> target = writeTargetNamed(names, name);
  if (!target) return {false, {}, refusalFor(names, name)};
  if (target->control.kind != WriteControl::Kind::kBits) {
    return {false, {}, target->name + " is a value, not a bit field — write it as a number"};
  }
  const WritableBit* bit = nullptr;
  for (const WritableBit& candidate : target->control.bits) {
    if (candidate.key == bitKey) {
      bit = &candidate;
      break;
    }
  }
  if (bit == nullptr) {
    std::string offered;
    for (std::size_t at = 0; at < target->control.bits.size(); at++) {
      if (at > 0) offered += ", ";
      offered += target->control.bits[at].key;
    }
    return {false, {}, target->name + " has no writable bit called “" + std::string(bitKey) + "”. Offered: " + offered};
  }
  if (std::floor(currentValue) != currentValue || currentValue < 0 || currentValue > 0xffff) {
    return {false, {}, "the current " + target->name + " reads " + text::jsNumber(currentValue) +
                           ", which is not a 16-bit word"};
  }
  const int current = static_cast<int>(currentValue);
  const int next = on ? (current | bit->mask) & 0xffff : (current & ~bit->mask) & 0xffff;
  const int changed = (next ^ current) & 0xffff;
  if (changed != 0 && changed != bit->mask) {
    // Unreachable by the arithmetic; the whole bit-toggle design exists to make this assertion.
    char maskHex[16];
    char bitHex[16];
    std::snprintf(maskHex, sizeof maskHex, "0x%x", changed);
    std::snprintf(bitHex, sizeof bitHex, "0x%x", bit->mask);
    return {false, {},
            "vcu-write: a " + bit->key + " toggle would change mask " + maskHex + " of " + target->name + ", not only " +
                bitHex};
  }
  return buildPlan(names, *target, next, currentValue);
}

std::optional<std::string> writeTargetProblemIn(const ParameterNames& names, const WriteTarget& target,
                                                const VcuParameterTable& bikeTable) {
  const VcuParameter* onBike = bikeTable.atIndex(target.index);
  if (onBike == nullptr) {
    return "index " + std::to_string(target.index) + " (“" + target.name + "”) is not in table " +
           std::to_string(bikeTable.tableType) + " at all";
  }
  if (upper(onBike->name()) != upper(target.name)) {
    return "writing “" + target.name + "” would go to index " + std::to_string(target.index) + ", which table " +
           std::to_string(bikeTable.tableType) + " calls “" + std::string(onBike->name()) +
           "” — that is a different calibration cell";
  }
  const VcuParameter* encoding = names.atIndex(target.index);
  if (encoding == nullptr || encoding->type != onBike->type || encoding->isSigned != onBike->isSigned ||
      encoding->micro != onBike->micro) {
    const std::string encoded =
        encoding != nullptr ? std::string(storageTypeName(encoding->type)) + " " + (encoding->isSigned ? "S" : "U") +
                                  " on " + microName(encoding->micro)
                            : "nothing at all";
    return target.name + " would be encoded as " + encoded + ", but table " + std::to_string(bikeTable.tableType) +
           " stores it as " + storageTypeName(onBike->type) + " " + (onBike->isSigned ? "S" : "U") + " on " +
           microName(onBike->micro);
  }
  return std::nullopt;
}

std::vector<std::string> allowlistProblemsIn(const ParameterNames& names, const VcuParameterTable& bikeTable) {
  std::vector<std::string> problems;
  for (const WriteTarget& target : curatedWriteTargets()) {
    const VcuParameter* onBike = bikeTable.atIndex(target.index);
    if (onBike == nullptr) {
      problems.push_back("index " + std::to_string(target.index) + " (“" + target.name + "”) is not in table " +
                         std::to_string(bikeTable.tableType) + " at all");
      continue;
    }
    if (upper(onBike->name()) != upper(target.name)) {
      problems.push_back("index " + std::to_string(target.index) + " is “" + target.name + "” here and “" +
                         std::string(onBike->name()) + "” in table " + std::to_string(bikeTable.tableType));
      continue;
    }
    const VcuParameter* encoding = names.atIndex(target.index);
    if (encoding == nullptr || encoding->type != onBike->type || encoding->isSigned != onBike->isSigned ||
        encoding->micro != onBike->micro) {
      const std::string encoded =
          encoding != nullptr ? std::string(storageTypeName(encoding->type)) + " " + (encoding->isSigned ? "S" : "U") +
                                    " on " + microName(encoding->micro)
                              : "nothing at all";
      problems.push_back(target.name + " (index " + std::to_string(target.index) + ") would be encoded as " + encoded +
                         " from table " + std::to_string(names.active().tableType) + ", but table " +
                         std::to_string(bikeTable.tableType) + " stores it as " + storageTypeName(onBike->type) + " " +
                         (onBike->isSigned ? "S" : "U") + " on " + microName(onBike->micro));
    }
  }
  return problems;
}

bool isAllowedPlan(const ParameterNames& names, const ParameterWritePlan& plan) {
  const std::optional<WriteTarget> target = writeTargetNamed(names, plan.name);
  if (!target) return false;
  const WritePlanOutcome rebuilt = target->control.kind == WriteControl::Kind::kNumber
                                       ? planWrite(names, plan.name, plan.value, plan.previousValue)
                                       : rebuildBitPlan(names, *target, plan.value, plan.previousValue);
  if (!rebuilt.ok) return false;
  const ParameterWritePlan& expected = rebuilt.plan;
  if (expected.identifier != plan.identifier || expected.index != plan.index || expected.micro != plan.micro) {
    return false;
  }
  if (expected.record.size() != plan.record.size()) return false;
  return std::equal(expected.record.begin(), expected.record.end(), plan.record.begin());
}

std::optional<std::string> planTableProblem(const ParameterNames& names, const ParameterWritePlan& plan,
                                            const TableTypeReport* report, bool reportPresent) {
  const TableGateVerdict verdict = evaluateTableGate(reportPresent ? report : nullptr, names);
  if (!verdict.tableType) return std::nullopt;  // the gate refuses on its own
  const VcuParameterTable* bikeTable = parameterTableFor(*verdict.tableType);
  if (bikeTable == nullptr) {
    return "the bike names table " + std::to_string(*verdict.tableType) + ", which this software cannot rebuild";
  }
  const std::optional<WriteTarget> target = writeTargetNamed(names, plan.name);
  if (!target) return "“" + plan.name + "” is not a writable parameter";
  return writeTargetProblemIn(names, *target, *bikeTable);
}

}  // namespace emcool::vcu
