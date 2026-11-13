#include "emcool/vcu/table_gate.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/write_targets.hpp"

namespace emcool::vcu {

namespace {

std::string byteHex(int value) {
  char text[4];
  std::snprintf(text, sizeof text, "%02X", value & 0xff);
  return text;
}

bool sameTable(int left, int right) {
  if (left == right) return true;
  const std::vector<int> twins = contentTwinsOf(left);
  return std::find(twins.begin(), twins.end(), right) != twins.end();
}

std::string identify(const ParameterNames& names, int index) {
  const VcuParameter* parameter = names.atIndex(index);
  if (parameter == nullptr) return "parameter " + std::to_string(index);
  return std::string(parameter->name()) + " (" + std::to_string(index) + ", " + microName(parameter->micro) + ")";
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += sep;
    out += part;
  }
  return out;
}

std::string describeMismatch(const ParameterNames& names, int index, double value) {
  const std::optional<TableTypeVerdict> verdict = names.checkTableType(index, value);
  if (verdict) return verdict->message;
  return identify(names, index) + " named " + describeTableType(value) + ".";
}

std::string describeMismatchRemedy(const ParameterNames& names, const std::vector<int>& indices) {
  std::vector<std::string> who;
  for (const int index : indices) who.push_back(identify(names, index));
  return "⚠️ No read clears " + join(who, " and ") +
         " — the bike answered, and the answer is the problem. Writing by index means trusting this software's name "
         "for that index, and on the table it named the name may belong to a different parameter entirely. ✅ This is "
         "fixable by you and does not need a code change: add the table as docs/service-mode.md (\"Parameter "
         "tables\") describes and run tools/generate-vcu-tables.mts, and this gate opens. Carried today: " +
         knownTableTypesText() + ".";
}

constexpr const char* kSplitRemedy =
    "⚠️ No read clears this either, and picking one micro's answer is exactly the wrong move: half the ids would then "
    "be named out of a table that does not describe them. Nothing here holds one table per micro today, and adding "
    "that is a deliberate change to src/vcu/param-table.ts rather than something to bodge at the write button. Reading "
    "is unaffected, and both "
    "tables are carried, so the parameter page can be read under either. Please open an issue quoting both numbers — a "
    "genuinely split VCU is worth knowing about, and how far apart the two tables actually are decides how much work "
    "supporting it is.";

std::string describeSplit(const ParameterNames& names, const std::map<int, int>& named) {
  std::vector<std::string> who;
  for (const auto& [index, value] : named) who.push_back(identify(names, index) + " says " + describeTableType(value));
  return "*** The two micros name DIFFERENT parameter tables: " + join(who, ", and ") +
         ". *** Both are carried, so this is not a gap in this software — it is a claim about the bike. They hold "
         "separate EEPROMs and are asked separately, so they can genuinely have been flashed with different tables.";
}

std::string describeUnusable(const ParameterNames& names, int index) {
  return identify(names, index) +
         " answered with a record whose width contradicts the name table, so it named no table. That is not the same "
         "as unread — it replied — and a wrong-width record here puts the framing of every other value in that sweep "
         "in question too.";
}

std::string describeUnread(const ParameterNames& names, int index) {
  const VcuParameter* parameter = names.atIndex(index);
  const char* micro = parameter != nullptr ? microName(parameter->micro) : "micro";
  return identify(names, index) + " has never been read, so nothing confirms the " + micro + "'s parameter names are " +
         describeTableType(names.active().tableType) +
         "'s, which is what is being shown. The two micros hold separate EEPROMs and are asked separately; one "
         "answering is not confirmation of the other.";
}

std::string sweepOrderCaveatFor(const ParameterNames& names, int index) {
  const VcuParameter* parameter = names.atIndex(index);
  if (parameter != nullptr && parameter->micro == Micro::kA8) {
    return "The A8 is swept second and " + std::to_string(index) +
           " is at the very end of it, so a sweep the safety gate cut short is unlikely to have reached it — check it "
           "says it finished.";
  }
  return "A sweep reaches " + std::to_string(index) + " partway through the A9, which is the half it does first.";
}

std::string readInstructionFor(const ParameterNames& names, int index) {
  const VcuParameter* parameter = names.atIndex(index);
  const char* micro = parameter != nullptr ? microName(parameter->micro) : "the owning micro";
  const int identifier = parameter != nullptr ? parameter->identifier() : 0;
  const std::string request = "22 " + byteHex(identifier >> 8) + " " + byteHex(identifier & 0xff);
  const std::string paramName = parameter != nullptr ? std::string(parameter->name()) : "?";
  return "Read parameter " + std::to_string(index) + " " + paramName + " on the " + micro + " — " + request +
         " in a 10 81 session: read-only, no SecurityAccess, one frame. Any of the tables this software carries is a "
         "good answer (" +
         knownTableTypesText() + "); anything else is answered by the remedy above. ⚠️ It has to be RECORDED, not "
         "merely seen: this gate reads the last parameter sweep's snapshot, so run Service mode → read the parameters "
         "and let it finish. " +
         sweepOrderCaveatFor(names, index) +
         " The /vcu-probe endpoint shows the same answer in one frame — curl -X POST -H 'X-Emcool: service-mode' "
         "'<pi>/vcu-probe?target=" +
         micro + "&bank=1&index=" + std::to_string(index) + "' — but it stores nothing, so it cannot open this gate.";
}

}  // namespace

const char* tableGateStateName(TableGateState state) {
  switch (state) {
    case TableGateState::kConfirmed:
      return "confirmed";
    case TableGateState::kMismatched:
      return "mismatched";
    case TableGateState::kSplit:
      return "split";
    case TableGateState::kUnwritable:
      return "unwritable";
    case TableGateState::kUnusable:
      return "unusable";
    case TableGateState::kUnread:
      return "unread";
  }
  return "unread";
}

TableGateVerdict evaluateTableGate(const TableTypeReport* report, const ParameterNames& names) {
  std::map<int, int> named;
  std::map<int, int> answeredWrongly;
  if (report != nullptr) {
    for (const TableTypeVerdict& verdict : report->verdicts) {
      const std::optional<TableTypeVerdict> rechecked = names.checkTableType(verdict.index, verdict.value);
      if (rechecked && rechecked->recognised) {
        named[verdict.index] = static_cast<int>(verdict.value);
      } else if (rechecked) {
        answeredWrongly[verdict.index] = static_cast<int>(verdict.value);
      }
    }
  }
  std::vector<int> outstanding;
  for (const int index : kTableTypeIndices) {
    if (named.find(index) == named.end()) outstanding.push_back(index);
  }
  std::vector<int> values;
  for (const auto& [index, value] : named) values.push_back(value);
  std::optional<int> agreed;
  if (answeredWrongly.empty() && !values.empty()) {
    const bool allSame = std::all_of(values.begin(), values.end(), [&](int v) { return sameTable(v, values.front()); });
    if (allSame) agreed = values.front();
  }

  TableGateVerdict result;
  result.outstanding = outstanding;
  result.tableType = agreed;

  if (outstanding.empty() && agreed) {
    const VcuParameterTable* table = parameterTableFor(*agreed);
    std::vector<std::string> problems =
        table != nullptr ? allowlistProblemsIn(names, *table) : std::vector<std::string>{"the table could not be rebuilt at all"};
    if (problems.empty()) {
      result.state = TableGateState::kConfirmed;
      result.writesAllowed = true;
      result.reason = "Both micros name " + describeTableType(*agreed) + ", which is a table this software carries.";
      return result;
    }
    result.state = TableGateState::kUnwritable;
    result.noReadWillHelp = true;
    result.reason = "Both micros name " + describeTableType(*agreed) + ", which this software carries — but " +
                    std::to_string(problems.size()) +
                    " of the parameters on the write allowlist do not mean there what they mean here: " +
                    join(problems, "; ") + ".";
    result.remedy =
        "⚠️ No read clears this and the bike is not at fault. write_targets pairs each writable parameter with an "
        "INDEX, and on this bike's table that index is a different parameter. Writing anyway would send the value to "
        "the right micro with the right width and change the wrong calibration cell. The allowlist needs an entry that "
        "is correct for this table before any of it can be offered here.";
    return result;
  }

  std::vector<int> mismatched;
  std::vector<int> unusable;
  std::vector<int> unread;
  for (const int index : outstanding) {
    if (answeredWrongly.find(index) != answeredWrongly.end()) {
      mismatched.push_back(index);
      continue;
    }
    const bool reportUnusable =
        report != nullptr && std::find(report->unusable.begin(), report->unusable.end(), index) != report->unusable.end();
    if (reportUnusable) {
      unusable.push_back(index);
    } else {
      unread.push_back(index);
    }
  }
  const bool split = !agreed && values.size() > 1;

  result.state = split                 ? TableGateState::kSplit
                 : !mismatched.empty()  ? TableGateState::kMismatched
                 : !unusable.empty()    ? TableGateState::kUnusable
                                        : TableGateState::kUnread;
  std::vector<std::string> reasons;
  if (split) reasons.push_back(describeSplit(names, named));
  for (const int index : mismatched) reasons.push_back(describeMismatch(names, index, answeredWrongly[index]));
  for (const int index : unusable) reasons.push_back(describeUnusable(names, index));
  for (const int index : unread) reasons.push_back(describeUnread(names, index));
  std::vector<std::string> remedies;
  if (split) remedies.push_back(kSplitRemedy);
  if (!mismatched.empty()) remedies.push_back(describeMismatchRemedy(names, mismatched));
  for (const int index : unusable) remedies.push_back(readInstructionFor(names, index));
  for (const int index : unread) remedies.push_back(readInstructionFor(names, index));

  result.writesAllowed = false;
  result.noReadWillHelp = split || !mismatched.empty();
  result.reason = join(reasons, " ");
  result.remedy = join(remedies, " ");
  return result;
}

void writeTableGateVerdictJson(text::TextBuffer& out, const TableGateVerdict& verdict) {
  out.append("{\"state\":");
  out.appendJsonString(tableGateStateName(verdict.state));
  out.append(verdict.writesAllowed ? ",\"writesAllowed\":true" : ",\"writesAllowed\":false");
  out.append(verdict.noReadWillHelp ? ",\"noReadWillHelp\":true,\"reason\":" : ",\"noReadWillHelp\":false,\"reason\":");
  out.appendJsonString(verdict.reason);
  out.append(",\"remedy\":");
  out.appendJsonString(verdict.remedy);
  out.append(",\"outstanding\":[");
  for (std::size_t at = 0; at < verdict.outstanding.size(); at++) {
    if (at > 0) out.append(",");
    out.appendInteger(verdict.outstanding[at]);
  }
  out.append("],\"tableType\":");
  if (verdict.tableType) {
    out.appendInteger(*verdict.tableType);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace emcool::vcu
