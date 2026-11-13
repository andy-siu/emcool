#include "emcool/vcu/catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "emcool/log.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-table";

// Number.prototype.toString(16), upper-cased, padded on the left as padStart does.
std::string paddedHex(std::int64_t value, std::size_t width) {
  char digits[24];
  std::snprintf(digits, sizeof digits, "%s%llX", value < 0 ? "-" : "", static_cast<unsigned long long>(std::llabs(value)));
  std::string text = digits;
  if (text.size() < width) text.insert(0, width - text.size(), '0');
  return text;
}

std::string joinNumbers(const std::vector<int>& numbers) {
  std::string text;
  for (const int number : numbers) {
    if (!text.empty()) text += ", ";
    text += std::to_string(number);
  }
  return text;
}

int countDifferences(const VcuParameterTable& from, const VcuParameterTable& to) {
  int differing = 0;
  for (const VcuParameter& parameter : to.parameters) {
    const VcuParameter* before = from.atIndex(parameter.index);
    if (before == nullptr || before->name() != parameter.name() || before->isSigned != parameter.isSigned) differing++;
  }
  return differing;
}

}  // namespace

const VcuParameter* VcuParameterTable::atIndex(int index) const {
  const auto found = std::lower_bound(parameters.begin(), parameters.end(), index,
                                      [](const VcuParameter& parameter, int wanted) { return parameter.index < wanted; });
  return found != parameters.end() && found->index == index ? &*found : nullptr;
}

const VcuParameterTable* parameterTableFor(int tableType) {
  for (const VcuParameterTable& table : parameterTables()) {
    if (table.tableType == tableType) return &table;
  }
  return nullptr;
}

bool isTableTypeIndex(int index) { return index == kTableTypeIndices[0] || index == kTableTypeIndices[1]; }

std::string describeTableType(double raw) {
  // As JavaScript's >> and & do, work on the value as a 32-bit integer.
  const auto integer = static_cast<std::int64_t>(raw);
  const auto bits = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(integer)));
  const int family = (bits >> 12) & 0xf;
  const int revision = bits & 0xfff;
  return jsNumber(raw) + " (0x" + paddedHex(integer, 4) + " — family " + std::to_string(family) + ", revision 0x" +
         paddedHex(revision, 3) + ")";
}

std::vector<int> contentTwinsOf(int tableType) {
  const VcuParameterTable* self = parameterTableFor(tableType);
  std::vector<int> twins;
  if (self == nullptr) return twins;
  for (const VcuParameterTable& table : parameterTables()) {
    if (table.tableType != tableType && std::strcmp(table.fingerprint, self->fingerprint) == 0) {
      twins.push_back(table.tableType);
    }
  }
  std::sort(twins.begin(), twins.end());
  return twins;
}

std::string knownTableTypesText() {
  std::vector<int> types;
  for (const VcuParameterTable& table : parameterTables()) types.push_back(table.tableType);
  return joinNumbers(types);
}

const A8FirmwareRow* firmwareRowFor(Micro micro, int bank, int index) {
  if (micro != Micro::kA8 || bank != kCalibrationBank) return nullptr;
  for (const A8FirmwareRow& row : a8FirmwareRows()) {
    if (row.index == index) return &row;
  }
  return nullptr;
}

std::string describeFirmwareRow(const A8FirmwareRow& row) {
  const std::string known = row.known != nullptr ? row.known : "nothing traced beyond the row and its width";
  return "not in params.ecf — A8's own firmware table types index " + std::to_string(row.index) + " " +
         storageTypeName(row.type) + "; " + known + " (docs/vcu-parameters.md §2)";
}

ParameterNames::ParameterNames() : active_(parameterTableFor(kDefaultTableType)) {}

ParameterNames::Selection ParameterNames::select(int tableType) {
  if (tableType == active_->tableType) {
    return {true, tableType, "vcu-table: already using " + describeTableType(tableType)};
  }
  const VcuParameterTable* table = parameterTableFor(tableType);
  if (table == nullptr) {
    // A new table comes in through tools/generate-vcu-tables.mts, not an extract script.
    const std::string message = "vcu-table: ⚠️  this bike reports " + describeTableType(tableType) +
                                ", which this software does not carry — keeping " +
                                describeTableType(active_->tableType) +
                                "'s names, which may be WRONG for this bike. Add the table as docs/service-mode.md "
                                "(\"Parameter tables\") describes and run tools/generate-vcu-tables.mts. "
                                "Carried: " +
                                knownTableTypesText() + ".";
    logWarning(kTag, "%s", message.c_str() + std::strlen("vcu-table: "));
    return {false, tableType, message};
  }
  const int differing = countDifferences(*active_, *table);
  const VcuParameterTable* previous = active_;
  active_ = table;
  const std::string message = "vcu-table: now naming parameters from " + describeTableType(table->tableType) +
                              " (Energica bundle " + table->exportStamp + "), was " +
                              describeTableType(previous->tableType) + " — " + std::to_string(differing) + " of " +
                              std::to_string(table->parameters.size()) +
                              " ids are named differently between the two";
  logInfo(kTag, "%s", message.c_str() + std::strlen("vcu-table: "));
  return {true, tableType, message};
}

std::optional<TableTypeVerdict> ParameterNames::checkTableType(int index, double value) const {
  const VcuParameter* parameter = atIndex(index);
  if (parameter == nullptr || !isTableTypeIndex(index)) return std::nullopt;
  const std::string identity =
      std::string(parameter->name()) + " (" + std::to_string(index) + ", " + microName(parameter->micro) + ")";
  const bool integral = std::floor(value) == value && std::fabs(value) < 1e9;
  const VcuParameterTable* table = integral ? parameterTableFor(static_cast<int>(value)) : nullptr;
  if (table != nullptr) {
    const std::vector<int> twins = contentTwinsOf(table->tableType);
    std::string message = identity + " = " + describeTableType(value) + " — carried, from Energica bundle " +
                          table->exportStamp;
    if (!twins.empty()) message += " (identical content to " + joinNumbers(twins) + ")";
    if (table->tableType != active_->tableType) {
      message += "; names are currently being shown from " + describeTableType(active_->tableType);
    }
    return TableTypeVerdict{index, parameter->micro, value, true, message};
  }
  const char* micro = microName(parameter->micro);
  const std::string message =
      "*** " + identity + " = " + describeTableType(value) + ", which this software does NOT carry. *** Every parameter " +
      "NAME shown for the " + micro + " is " + describeTableType(active_->tableType) +
      "'s and may belong to a different parameter — routing and record widths are the same across all of Energica's "
      "tables, so a wrong name still reads and still writes cleanly. On 20 of the 28 tables shipped in 2024, ids 70–94 "
      "are a regen curve rather than the battery cell block, so this is not a small difference. Do not write anything "
      "by name until the table is added: add it as docs/service-mode.md's Parameter tables section describes and "
      "run tools/generate-vcu-tables.mts. "
      "Carried today: " +
      knownTableTypesText() + ".";
  return TableTypeVerdict{index, parameter->micro, value, false, message};
}

}  // namespace emcool::vcu
