#include "emcool/vcu/snapshot.hpp"

#include <algorithm>
#include <cctype>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kFirmwareTableName = "A8's firmware table";

std::string padStart(std::string text, std::size_t width) {
  if (text.size() < width) text.insert(0, width - text.size(), ' ');
  return text;
}

std::string padEnd(std::string text, std::size_t width) {
  if (text.size() < width) text.append(width - text.size(), ' ');
  return text;
}

std::string describeFailure(const VcuReadResult& outcome) {
  switch (outcome.status) {
    case ReadStatus::kRefused:
      return "refused with NRC " + outcome.description;
    case ReadStatus::kNoResponse:
      return "no reply in an open session — not the same claim as “no such parameter”";
    case ReadStatus::kNoSession:
    case ReadStatus::kStalled:
    case ReadStatus::kUnrecognised:
      return outcome.reason;
    case ReadStatus::kAbandoned:
      return "the reply was discarded rather than decoded: " + outcome.reason;
    case ReadStatus::kNotSent:
      return "never asked — " + outcome.reason;
    case ReadStatus::kRead:
      break;
  }
  return readStatusName(outcome.status);
}

std::optional<RecordEncoding> encodingOf(const VcuParameter* parameter, const A8FirmwareRow* firmware) {
  if (parameter != nullptr) return RecordEncoding{parameter->type, parameter->isSigned, nullptr};
  if (firmware != nullptr) return RecordEncoding{firmware->type, std::nullopt, kFirmwareTableName};
  return std::nullopt;
}

std::optional<std::string> sectionOf(const VcuParameter* parameter) {
  if (parameter == nullptr || parameter->section() == nullptr) return std::nullopt;
  return std::string(parameter->section());
}

std::optional<double> otherBikeValueOf(const VcuParameter* parameter) {
  if (parameter == nullptr || !parameter->hasOtherBikeValue) return std::nullopt;
  return parameter->otherBikeValue;
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

std::string identityOf(const VcuParameter* parameter, int index) {
  if (parameter == nullptr) return "parameter " + std::to_string(index);
  return std::string(parameter->name()) + " (" + std::to_string(index) + ", " + microName(parameter->micro) + ")";
}

std::string describeSplit(const std::vector<TableTypeVerdict>& verdicts) {
  std::string named;
  for (const TableTypeVerdict& verdict : verdicts) {
    if (!verdict.recognised) continue;
    if (!named.empty()) named += " and ";
    named += std::string("the ") + microName(verdict.micro) + " says " + describeTableType(verdict.value);
  }
  return "*** The two micros name DIFFERENT parameter tables: " + named +
         ". *** They hold separate EEPROMs and are asked separately, so this can be true of the bike rather than a "
         "fault — but no single set of names is then right for the whole VCU, and this software does not carry a "
         "per-micro table. Reading is unaffected; writing is refused until they agree or until src/vcu/param-table.ts "
         "learns to hold one table per micro.";
}

std::optional<double> agreedTableType(const std::vector<TableTypeVerdict>& verdicts) {
  std::vector<const TableTypeVerdict*> recognised;
  for (const TableTypeVerdict& verdict : verdicts) {
    if (!verdict.recognised) return std::nullopt;
    recognised.push_back(&verdict);
  }
  if (recognised.empty()) return std::nullopt;
  const TableTypeVerdict& first = *recognised.front();
  const std::vector<int> twins = contentTwinsOf(static_cast<int>(first.value));
  for (std::size_t at = 1; at < recognised.size(); at++) {
    const double value = recognised[at]->value;
    if (value != first.value && std::find(twins.begin(), twins.end(), value) == twins.end()) return std::nullopt;
  }
  for (const TableTypeVerdict* verdict : recognised) {
    if (verdict->index == 276) return verdict->value;
  }
  return first.value;
}

}  // namespace

const char* readStatusName(ReadStatus status) {
  switch (status) {
    case ReadStatus::kRead:
      return "read";
    case ReadStatus::kRefused:
      return "refused";
    case ReadStatus::kNoResponse:
      return "no-response";
    case ReadStatus::kNoSession:
      return "no-session";
    case ReadStatus::kStalled:
      return "stalled";
    case ReadStatus::kAbandoned:
      return "abandoned";
    case ReadStatus::kUnrecognised:
      return "unrecognised";
    case ReadStatus::kNotSent:
      return "not-sent";
  }
  return "not-sent";
}

std::optional<ReadStatus> readStatusNamed(std::string_view name) {
  for (const ReadStatus status : kReadStatuses) {
    if (name == readStatusName(status)) return status;
  }
  return std::nullopt;
}

ArrivalLatency arrivalLatencyMs(std::optional<double> arrivalMs, double nowMs) {
  if (!arrivalMs) return {false, 0, "the kernel supplied no arrival timestamp for this frame"};
  const double ms = nowMs - *arrivalMs;
  if (ms < 0) return {false, 0, "arrived " + jsToFixed(-ms, 1) + " ms in the future — the clock stepped"};
  if (ms > 5000) return {false, 0, jsToFixed(ms / 1000, 1) + " s is not a dispatch delay — the clock stepped"};
  return {true, ms, {}};
}

void writeLatencyJson(text::TextBuffer& out, const std::optional<ArrivalLatency>& latency) {
  if (!latency) {
    out.append("null");
  } else if (latency->known) {
    out.append("{\"known\":true,\"ms\":");
    out.appendNumber(latency->ms);
    out.append("}");
  } else {
    out.append("{\"known\":false,\"reason\":");
    out.appendJsonString(latency->reason);
    out.append("}");
  }
}

std::optional<std::string> note(std::initializer_list<std::optional<std::string>> parts) {
  std::string said;
  for (const std::optional<std::string>& part : parts) {
    if (!part || part->empty()) continue;
    if (!said.empty()) said += " — ";
    said += *part;
  }
  if (said.empty()) return std::nullopt;
  return said;
}

VcuParameterRow toParameterRow(const VcuReadOutcome& outcome, const ParameterNames& names) {
  const VcuParameter* parameter = names.atIndex(outcome.index);
  const A8FirmwareRow* firmware =
      parameter != nullptr ? nullptr : firmwareRowFor(outcome.micro, kCalibrationBank, outcome.index);
  VcuParameterRow row;
  row.index = outcome.index;
  row.identifier = outcome.identifier;
  row.micro = outcome.micro;
  if (parameter != nullptr) {
    row.name = std::string(parameter->name());
    row.section = sectionOf(parameter);
    row.type = parameter->type;
    row.isSigned = parameter->isSigned;
  } else if (firmware != nullptr) {
    row.type = firmware->type;
  }
  row.status = outcome.status;
  row.otherBikeValue = otherBikeValueOf(parameter);
  const std::optional<std::string> known =
      firmware != nullptr ? std::optional<std::string>(describeFirmwareRow(*firmware)) : std::nullopt;
  if (outcome.status != ReadStatus::kRead) {
    row.note = note({describeFailure(outcome), known});
    return row;
  }
  const std::optional<RecordEncoding> encoding = encodingOf(parameter, firmware);
  const VcuParameterValue interpreted = interpretRecord(outcome.record, encoding);
  row.rawHex = interpreted.rawHex;
  row.unsignedValue = interpreted.unsignedValue;
  row.value = interpreted.value;
  row.widthMismatch = interpreted.widthMismatch;
  row.note = note({interpreted.widthMismatch && encoding
                       ? std::optional<std::string>(describeWidthMismatch(outcome.record.size(), *encoding))
                       : std::nullopt,
                   known});
  return row;
}

std::string describeRow(const VcuParameterRow& row) {
  const std::string name = padStart(std::to_string(row.index), 4) + " " + padEnd(row.name.value_or("?"), 30);
  const std::string noted = row.note ? " — " + *row.note : "";
  if (row.status != ReadStatus::kRead) return name + " " + readStatusName(row.status) + noted;
  const std::string value =
      row.value ? jsNumber(*row.value) : "raw " + (row.unsignedValue ? jsNumber(*row.unsignedValue) : "null");
  const std::string comparison = !row.otherBikeValue || !row.value || *row.otherBikeValue == *row.value
                                     ? ""
                                     : "   (the other bike's file says " + jsNumber(*row.otherBikeValue) + ")";
  return name + " " + padStart(value, 8) + "  [" + row.rawHex.value_or("null") + "]" + comparison + noted;
}

void writeRowJson(text::TextBuffer& out, const VcuParameterRow& row) {
  out.append("{");
  if (row.at) {
    out.append("\"at\":");
    out.appendNumber(*row.at);
    out.append(",");
  }
  out.append("\"index\":");
  out.appendInteger(row.index);
  out.append(",\"identifier\":");
  out.appendInteger(row.identifier);
  out.append(",\"micro\":");
  out.appendJsonString(microName(row.micro));
  out.append(",\"name\":");
  writeOptionalString(out, row.name);
  out.append(",\"section\":");
  writeOptionalString(out, row.section);
  out.append(",\"type\":");
  if (row.type) {
    out.appendJsonString(storageTypeName(*row.type));
  } else {
    out.append("null");
  }
  out.append(",\"signed\":");
  out.append(!row.isSigned ? "null" : *row.isSigned ? "true" : "false");
  out.append(",\"status\":");
  out.appendJsonString(readStatusName(row.status));
  out.append(",\"otherBikeValue\":");
  writeOptionalNumber(out, row.otherBikeValue);
  out.append(",\"rawHex\":");
  writeOptionalString(out, row.rawHex);
  out.append(",\"unsigned\":");
  writeOptionalNumber(out, row.unsignedValue);
  out.append(",\"value\":");
  writeOptionalNumber(out, row.value);
  out.append(row.widthMismatch ? ",\"widthMismatch\":true" : ",\"widthMismatch\":false");
  out.append(",\"note\":");
  writeOptionalString(out, row.note);
  out.append("}");
}

std::optional<VcuParameterRow> parseRowJson(std::string_view json) {
  ble::JsonObjectReader reader;
  std::vector<char> scratch(json.size() + 16);
  if (!reader.read(json, scratch)) return std::nullopt;
  VcuParameterRow row;
  const auto number = [&](std::string_view key) -> std::optional<double> {
    const ble::JsonMember* member = reader.find(key);
    if (member == nullptr || member->type != ble::JsonType::kNumber) return std::nullopt;
    return member->number;
  };
  const auto string = [&](std::string_view key) -> std::optional<std::string> {
    const ble::JsonMember* member = reader.find(key);
    if (member == nullptr || member->type != ble::JsonType::kString) return std::nullopt;
    return std::string(member->text);
  };
  const std::optional<double> index = number("index");
  const std::optional<double> identifier = number("identifier");
  const std::optional<std::string> micro = string("micro");
  const std::optional<std::string> status = string("status");
  if (!index || !identifier || !micro || !status || !microNamed(*micro) || !readStatusNamed(*status)) return std::nullopt;
  row.at = number("at");
  row.index = static_cast<int>(*index);
  row.identifier = static_cast<int>(*identifier);
  row.micro = *microNamed(*micro);
  row.name = string("name");
  row.section = string("section");
  if (const std::optional<std::string> type = string("type")) {
    for (const StorageType candidate : {StorageType::kBool, StorageType::kByte, StorageType::kWord, StorageType::kDword}) {
      if (*type == storageTypeName(candidate)) row.type = candidate;
    }
  }
  if (const ble::JsonMember* isSigned = reader.find("signed"); isSigned != nullptr && isSigned->type == ble::JsonType::kBool) {
    row.isSigned = isSigned->text == "true";
  }
  row.status = *readStatusNamed(*status);
  row.otherBikeValue = number("otherBikeValue");
  row.rawHex = string("rawHex");
  row.unsignedValue = number("unsigned");
  row.value = number("value");
  if (const ble::JsonMember* mismatch = reader.find("widthMismatch"); mismatch != nullptr) {
    row.widthMismatch = mismatch->type == ble::JsonType::kBool && mismatch->text == "true";
  }
  row.note = string("note");
  return row;
}

std::optional<std::vector<std::uint8_t>> bytesFromHex(std::string_view rawHex) {
  std::vector<std::uint8_t> bytes;
  std::size_t at = 0;
  while (at < rawHex.size()) {
    while (at < rawHex.size() && std::isspace(static_cast<unsigned char>(rawHex[at]))) at++;
    if (at >= rawHex.size()) break;
    std::size_t end = at;
    while (end < rawHex.size() && !std::isspace(static_cast<unsigned char>(rawHex[end]))) end++;
    const std::string_view part = rawHex.substr(at, end - at);
    const bool valid = part.size() <= 2 && std::all_of(part.begin(), part.end(), [](char character) {
                         return std::isxdigit(static_cast<unsigned char>(character)) != 0;
                       });
    if (!valid) {
      logWarning("vcu-snapshot", "“%.*s” is not a hex record, so that row is served with no typed value",
                 static_cast<int>(rawHex.size()), rawHex.data());
      return std::nullopt;
    }
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(part), nullptr, 16)));
    at = end;
  }
  return bytes;
}

void writeHeaderJson(text::TextBuffer& out, const SnapshotHeader& header) {
  out.append("\"readAt\":");
  out.appendNumber(header.readAt);
  out.append(header.complete ? ",\"complete\":true,\"micros\":[" : ",\"complete\":false,\"micros\":[");
  for (std::size_t at = 0; at < header.micros.size(); at++) {
    if (at > 0) out.append(",");
    out.appendJsonString(microName(header.micros[at]));
  }
  out.append("]");
}

std::optional<SnapshotHeader> parseHeaderJson(std::string_view json) {
  ble::JsonObjectReader reader;
  std::vector<char> scratch(json.size() + 16);
  if (!reader.read(json, scratch)) return std::nullopt;
  const ble::JsonMember* readAt = reader.find("readAt");
  const ble::JsonMember* complete = reader.find("complete");
  const ble::JsonMember* micros = reader.find("micros");
  if (readAt == nullptr || readAt->type != ble::JsonType::kNumber || complete == nullptr ||
      complete->type != ble::JsonType::kBool || micros == nullptr || micros->type != ble::JsonType::kArray) {
    return std::nullopt;
  }
  SnapshotHeader header;
  header.readAt = readAt->number;
  header.complete = complete->text == "true";
  for (std::size_t at = micros->text.find('"'); at != std::string_view::npos; at = micros->text.find('"', at + 4)) {
    if (const std::optional<Micro> micro = microNamed(micros->text.substr(at + 1, 2))) header.micros.push_back(*micro);
  }
  return header;
}

TableTypeReport reportTableType(const std::function<const VcuParameterRow*(int index)>& rowAt,
                                const ParameterNames& names) {
  TableTypeReport report;
  std::vector<std::pair<int, const VcuParameterRow*>> unusable;
  for (const int index : kTableTypeIndices) {
    const VcuParameterRow* row = rowAt(index);
    if (row != nullptr && row->status != ReadStatus::kRead) row = nullptr;
    const std::optional<TableTypeVerdict> verdict =
        row == nullptr || !row->value ? std::nullopt : names.checkTableType(index, *row->value);
    if (verdict) {
      report.verdicts.push_back(*verdict);
    } else if (row != nullptr) {
      unusable.emplace_back(index, row);
    } else {
      report.unread.push_back(index);
    }
  }
  int recognised = 0;
  for (const TableTypeVerdict& verdict : report.verdicts) {
    if (!verdict.recognised) report.mismatched = true;
    if (verdict.recognised) recognised++;
  }
  report.tableType = agreedTableType(report.verdicts);
  report.split = !report.tableType && recognised > 1;
  if (report.split) report.lines.push_back("🚨  " + describeSplit(report.verdicts));
  for (const TableTypeVerdict& verdict : report.verdicts) {
    if (!verdict.recognised) report.lines.push_back("🚨  " + verdict.message);
  }
  for (const auto& [index, row] : unusable) {
    const VcuParameter* parameter = names.atIndex(index);
    const std::string expected = parameter != nullptr ? std::to_string(recordLengthFor(parameter->type)) + "-byte " +
                                                            storageTypeName(parameter->type)
                                                      : "2-byte WORD";
    const std::string got = row->rawHex ? "[" + *row->rawHex + "]" : "nothing";
    report.lines.push_back("🚨  *** " + identityOf(parameter, index) + " answered " + got +
                           ", which the name table says should be a " + expected +
                           ". *** It has named no table, and a wrong-width record here puts the framing of every other "
                           "value in this sweep in question.");
    report.unusable.push_back(index);
  }
  for (const int index : report.unread) {
    const VcuParameter* parameter = names.atIndex(index);
    const std::string owner = parameter != nullptr ? std::string("the ") + microName(parameter->micro) + "'s"
                                                   : "this micro's";
    report.lines.push_back("⚠️  " + identityOf(parameter, index) + " was not read, so nothing confirms " + owner +
                           " names are " + describeTableType(names.active().tableType) +
                           "'s, which is what is being shown. Reading it costs one frame in a 10 81 session.");
  }
  for (const TableTypeVerdict& verdict : report.verdicts) {
    if (verdict.recognised) report.lines.push_back("✅  " + verdict.message);
  }
  report.confirmed = report.unread.empty() && unusable.empty() && !report.mismatched && !report.split;
  return report;
}

void writeReportJson(text::TextBuffer& out, const TableTypeReport& report) {
  out.append("{\"verdicts\":[");
  for (std::size_t at = 0; at < report.verdicts.size(); at++) {
    const TableTypeVerdict& verdict = report.verdicts[at];
    if (at > 0) out.append(",");
    out.append("{\"index\":");
    out.appendInteger(verdict.index);
    out.append(",\"micro\":");
    out.appendJsonString(microName(verdict.micro));
    out.append(",\"value\":");
    out.appendNumber(verdict.value);
    out.append(verdict.recognised ? ",\"recognised\":true,\"message\":" : ",\"recognised\":false,\"message\":");
    out.appendJsonString(verdict.message);
    out.append("}");
  }
  out.append(report.confirmed ? "],\"confirmed\":true,\"tableType\":" : "],\"confirmed\":false,\"tableType\":");
  if (report.tableType) {
    out.appendNumber(*report.tableType);
  } else {
    out.append("null");
  }
  out.append(report.split ? ",\"split\":true,\"unread\":[" : ",\"split\":false,\"unread\":[");
  for (std::size_t at = 0; at < report.unread.size(); at++) {
    if (at > 0) out.append(",");
    out.appendInteger(report.unread[at]);
  }
  out.append("],\"unusable\":[");
  for (std::size_t at = 0; at < report.unusable.size(); at++) {
    if (at > 0) out.append(",");
    out.appendInteger(report.unusable[at]);
  }
  out.append(report.mismatched ? "],\"mismatched\":true,\"lines\":[" : "],\"mismatched\":false,\"lines\":[");
  for (std::size_t at = 0; at < report.lines.size(); at++) {
    if (at > 0) out.append(",");
    out.appendJsonString(report.lines[at]);
  }
  out.append("]}");
}

Retabler::Retabler(const TableTypeReport& report, const ParameterNames&)
    : agreed_(report.tableType ? parameterTableFor(static_cast<int>(*report.tableType)) : nullptr) {
  for (const TableTypeVerdict& verdict : report.verdicts) {
    const VcuParameterTable* table = verdict.recognised ? parameterTableFor(static_cast<int>(verdict.value)) : nullptr;
    const auto existing = std::find_if(byMicro_.begin(), byMicro_.end(),
                                       [&](const auto& entry) { return entry.first == verdict.micro; });
    if (existing != byMicro_.end()) {
      existing->second = table;
    } else {
      byMicro_.emplace_back(verdict.micro, table);
    }
  }
}

VcuParameterRow Retabler::retable(const VcuParameterRow& row) const {
  if (byMicro_.empty()) return row;
  const auto entry =
      std::find_if(byMicro_.begin(), byMicro_.end(), [&](const auto& candidate) { return candidate.first == row.micro; });
  const VcuParameterTable* table = entry != byMicro_.end() ? entry->second : agreed_;
  const bool contradicted = entry != byMicro_.end() && entry->second == nullptr && !isTableTypeIndex(row.index);
  if (table == nullptr && !contradicted) return row;

  const VcuParameter* parameter = table != nullptr ? table->atIndex(row.index) : nullptr;
  const std::optional<std::string> unnameable =
      contradicted ? std::optional<std::string>(std::string("the ") + microName(row.micro) +
                                                " named a parameter table this software does not carry, so nothing "
                                                "here can say what this index is called — the raw bytes are the bike's, "
                                                "the name is not available")
                   : std::nullopt;
  const A8FirmwareRow* firmware =
      parameter != nullptr || contradicted ? nullptr : firmwareRowFor(row.micro, kCalibrationBank, row.index);
  VcuParameterRow renamed = row;
  renamed.name = parameter != nullptr ? std::optional<std::string>(std::string(parameter->name())) : std::nullopt;
  renamed.section = sectionOf(parameter);
  renamed.type = parameter != nullptr  ? std::optional<StorageType>(parameter->type)
                 : firmware != nullptr ? std::optional<StorageType>(firmware->type)
                                       : std::nullopt;
  renamed.isSigned = parameter != nullptr ? std::optional<bool>(parameter->isSigned) : std::nullopt;
  renamed.otherBikeValue = otherBikeValueOf(parameter);
  if (!row.rawHex) {
    renamed.note = note({row.note, unnameable});
    return renamed;
  }
  const std::optional<std::vector<std::uint8_t>> record = bytesFromHex(*row.rawHex);
  if (!record) {
    renamed.unsignedValue.reset();
    renamed.value.reset();
    renamed.widthMismatch = false;
    renamed.note = note({row.note, unnameable,
                         "stored record “" + *row.rawHex + "” is not hex, so it could not be re-typed"});
    return renamed;
  }
  const std::optional<RecordEncoding> encoding = encodingOf(parameter, firmware);
  const VcuParameterValue interpreted = interpretRecord(*record, encoding);
  const std::optional<std::string> known =
      firmware != nullptr ? std::optional<std::string>(describeFirmwareRow(*firmware)) : std::nullopt;
  renamed.unsignedValue = interpreted.unsignedValue;
  renamed.value = interpreted.value;
  renamed.widthMismatch = interpreted.widthMismatch;
  renamed.note = note({interpreted.widthMismatch && encoding
                           ? std::optional<std::string>(describeWidthMismatch(record->size(), *encoding))
                           : std::nullopt,
                       unnameable, known});
  return renamed;
}

void diffSnapshots(const RowSource& previous, const RowSource& current,
                   const std::function<void(const VcuParameterChange&)>& emit) {
  std::optional<VcuParameterRow> before = previous();
  std::optional<VcuParameterRow> after = current();
  while (before || after) {
    if (after && (!before || after->index < before->index)) {
      emit({VcuParameterChange::Kind::kAppeared, after->index, after->name, after->micro, {}, {}, {}, {}});
      after = current();
      continue;
    }
    if (before && (!after || before->index < after->index)) {
      emit({VcuParameterChange::Kind::kDisappeared, before->index, before->name, before->micro, {}, {}, {}, {}});
      before = previous();
      continue;
    }
    if (before->status != after->status) {
      VcuParameterChange change{VcuParameterChange::Kind::kStatusChanged, after->index, after->name, after->micro,
                                {}, {}, {}, {}};
      change.fromStatus = before->status;
      change.toStatus = after->status;
      emit(change);
    } else if (before->rawHex && after->rawHex && *before->rawHex != *after->rawHex) {
      emit({VcuParameterChange::Kind::kValueChanged, after->index, after->name, after->micro, *before->rawHex,
            *after->rawHex, before->value, after->value});
    }
    before = previous();
    after = current();
  }
}

std::string describeChange(const VcuParameterChange& change) {
  const std::string who = std::to_string(change.index) + " " + change.name.value_or("(not in the name table)") +
                          " on " + microName(change.micro);
  const auto valueText = [](const std::optional<double>& value) { return value ? jsNumber(*value) : std::string("?"); };
  switch (change.kind) {
    case VcuParameterChange::Kind::kValueChanged:
      return "CHANGED  " + who + ": " + change.fromRaw + " → " + change.toRaw + "  (" + valueText(change.fromValue) +
             " → " + valueText(change.toValue) + ")";
    case VcuParameterChange::Kind::kStatusChanged:
      return "read     " + who + ": " + readStatusName(change.fromStatus) + " → " + readStatusName(change.toStatus) +
             " (about the read, not the parameter)";
    case VcuParameterChange::Kind::kAppeared:
      return "new      " + who + ": present in this snapshot only";
    case VcuParameterChange::Kind::kDisappeared:
      return "missing  " + who + ": present in the previous snapshot only";
  }
  return who;
}

}  // namespace emcool::vcu
