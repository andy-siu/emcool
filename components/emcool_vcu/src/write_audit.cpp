#include "emcool/vcu/write_audit.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr const char* kAuditFile = "service-writes.jsonl";

std::set<std::string> gReportedDamage;

void writeValue(text::TextBuffer& out, const AuditValue& value) {
  switch (value.kind) {
    case AuditValue::Kind::kAbsent:
    case AuditValue::Kind::kNull:
      out.append("null");
      return;
    case AuditValue::Kind::kNumber:
      out.appendNumber(value.number);
      return;
    case AuditValue::Kind::kString:
      out.appendJsonString(value.text);
      return;
  }
}

AuditValue valueFrom(const ble::JsonMember* member) {
  if (member == nullptr) return AuditValue::absent();
  switch (member->type) {
    case ble::JsonType::kNull:
      return AuditValue::null();
    case ble::JsonType::kNumber:
      return AuditValue::of(member->number);
    case ble::JsonType::kString:
      return AuditValue::of(std::string(member->text));
    default:
      return AuditValue::absent();
  }
}

std::optional<AuditAction> actionNamed(std::string_view name) {
  for (const AuditAction action :
       {AuditAction::kParameterWrite, AuditAction::kSetServicePoint, AuditAction::kClearDtcs, AuditAction::kRtcSync,
        AuditAction::kChargeCurrent, AuditAction::kChargeStop, AuditAction::kChargeSocLimit,
        AuditAction::kChargeSocLimitRead, AuditAction::kResetVcu, AuditAction::kReadServiceStamp}) {
    if (name == auditActionName(action)) return action;
  }
  return std::nullopt;
}

std::optional<AuditRecord> parseAuditLine(std::string_view line) {
  ble::JsonObjectReader reader;
  std::vector<char> scratch(line.size() + 16);
  if (!reader.read(line, scratch)) return std::nullopt;
  const ble::JsonMember* at = reader.find("at");
  const ble::JsonMember* action = reader.find("action");
  const ble::JsonMember* status = reader.find("status");
  if (at == nullptr || at->type != ble::JsonType::kNumber) return std::nullopt;
  if (action == nullptr || action->type != ble::JsonType::kString) return std::nullopt;
  if (status == nullptr || status->type != ble::JsonType::kString) return std::nullopt;
  const std::optional<AuditAction> parsedAction = actionNamed(action->text);
  if (!parsedAction) return std::nullopt;
  AuditRecord record;
  record.at = at->number;
  record.action = *parsedAction;
  record.status = std::string(status->text);
  if (const ble::JsonMember* trustworthy = reader.find("clockTrustworthy");
      trustworthy != nullptr && trustworthy->type == ble::JsonType::kBool) {
    record.clockTrustworthy = trustworthy->text == "true";
  }
  const auto string = [&](const char* key) -> std::optional<std::string> {
    const ble::JsonMember* member = reader.find(key);
    if (member == nullptr || member->type != ble::JsonType::kString) return std::nullopt;
    return std::string(member->text);
  };
  record.name = string("name");
  if (const ble::JsonMember* identifier = reader.find("identifier");
      identifier != nullptr && identifier->type == ble::JsonType::kNumber) {
    record.identifier = static_cast<int>(identifier->number);
  }
  record.micro = string("micro");
  record.before = valueFrom(reader.find("before"));
  record.after = valueFrom(reader.find("after"));
  record.requested = valueFrom(reader.find("requested"));
  record.rawHex = string("rawHex");
  record.note = string("note");
  record.runningVersion = string("runningVersion");
  return record;
}

std::string serialise(const AuditRecord& record) {
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  writeAuditRecordJson(measure, record);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  writeAuditRecordJson(out, record);
  text.resize(out.length());
  return text;
}

std::size_t leadingNulBytes(std::string_view line) {
  std::size_t count = 0;
  while (count < line.size() && line[count] == '\0') count++;
  return count;
}

bool isBlankOrNul(std::string_view line) {
  for (const char character : line) {
    if (character != '\0' && !std::isspace(static_cast<unsigned char>(character))) return false;
  }
  return true;
}

void warnOnce(const std::string& directory, std::size_t lineNumber, std::size_t length, const char* kind,
              const std::string& message) {
  const std::string key =
      directory + "|" + std::to_string(lineNumber) + "|" + std::to_string(length) + "|" + kind;
  if (gReportedDamage.count(key) != 0) return;
  gReportedDamage.insert(key);
  logWarning(kTag, "%s — said once per process", message.c_str());
}

}  // namespace

const char* auditActionName(AuditAction action) {
  switch (action) {
    case AuditAction::kParameterWrite:
      return "parameter-write";
    case AuditAction::kSetServicePoint:
      return "set-service-point";
    case AuditAction::kClearDtcs:
      return "clear-dtcs";
    case AuditAction::kRtcSync:
      return "rtc-sync";
    case AuditAction::kChargeCurrent:
      return "charge-current";
    case AuditAction::kChargeStop:
      return "charge-stop";
    case AuditAction::kChargeSocLimit:
      return "charge-soc-limit";
    case AuditAction::kChargeSocLimitRead:
      return "charge-soc-limit-read";
    case AuditAction::kResetVcu:
      return "reset-vcu";
    case AuditAction::kReadServiceStamp:
      return "read-service-stamp";
  }
  return "parameter-write";
}

void writeAuditRecordJson(text::TextBuffer& out, const AuditRecord& record) {
  out.append("{\"at\":");
  out.appendNumber(record.at);
  out.append(record.clockTrustworthy ? ",\"clockTrustworthy\":true" : ",\"clockTrustworthy\":false");
  out.append(",\"action\":");
  out.appendJsonString(auditActionName(record.action));
  out.append(",\"status\":");
  out.appendJsonString(record.status);
  if (record.name) {
    out.append(",\"name\":");
    out.appendJsonString(*record.name);
  }
  if (record.identifier) {
    out.append(",\"identifier\":");
    out.appendInteger(*record.identifier);
  }
  if (record.micro) {
    out.append(",\"micro\":");
    out.appendJsonString(*record.micro);
  }
  if (record.before.kind != AuditValue::Kind::kAbsent) {
    out.append(",\"before\":");
    writeValue(out, record.before);
  }
  if (record.after.kind != AuditValue::Kind::kAbsent) {
    out.append(",\"after\":");
    writeValue(out, record.after);
  }
  if (record.requested.kind != AuditValue::Kind::kAbsent) {
    out.append(",\"requested\":");
    writeValue(out, record.requested);
  }
  if (record.rawHex) {
    out.append(",\"rawHex\":");
    out.appendJsonString(*record.rawHex);
  }
  if (record.note) {
    out.append(",\"note\":");
    out.appendJsonString(*record.note);
  }
  if (record.runningVersion) {
    out.append(",\"runningVersion\":");
    out.appendJsonString(*record.runningVersion);
  }
  out.append("}");
}

void appendAuditRecord(const std::string& directory, AuditRecord record, const std::string& runningVersion) {
  record.runningVersion = runningVersion;
  const std::string text = serialise(record) + "\n";
  ::mkdir(directory.c_str(), 0775);
  const std::string path = directory + "/" + kAuditFile;
  std::FILE* file = std::fopen(path.c_str(), "ab");
  bool ok = file != nullptr;
  if (ok) {
    ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fflush(file) != 0) ok = false;
    std::fclose(file);
  }
  if (!ok) {
    logError(kTag, "========================================================================");
    logError(kTag, "COULD NOT RECORD %s (%s) IN THE AUDIT JOURNAL", auditActionName(record.action),
             record.status.c_str());
    logError(kTag, "the record that may not have reached the card: %s", serialise(record).c_str());
    logError(kTag, "the action itself already happened. Copy the line above somewhere by hand.");
    logError(kTag, "========================================================================");
  }
}

std::vector<AuditRecord> recentAuditRecords(const std::string& directory, std::size_t limit) {
  const std::string path = directory + "/" + kAuditFile;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  std::string content;
  char chunk[4096];
  std::size_t read = 0;
  while ((read = std::fread(chunk, 1, sizeof chunk, file)) > 0) content.append(chunk, read);
  std::fclose(file);

  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t at = 0; at <= content.size(); at++) {
    if (at == content.size() || content[at] == '\n') {
      lines.push_back(content.substr(start, at - start));
      start = at + 1;
    }
  }
  if (!content.empty() && content.back() == '\n') lines.pop_back();

  std::vector<AuditRecord> records;
  for (std::size_t position = 0; position < lines.size(); position++) {
    const std::string& line = lines[position];
    const std::size_t lineNumber = position + 1;
    if (isBlankOrNul(line)) {
      if (!line.empty() && line.find_first_not_of('\0') == std::string::npos) {
        warnOnce(directory, lineNumber, line.size(), "hole",
                 std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is " +
                     std::to_string(line.size()) + " NUL bytes — a record lost to a power cut, not a parse error");
      }
      continue;
    }
    const std::size_t hole = leadingNulBytes(line);
    if (hole > 0) {
      const std::string tail = line.substr(hole);
      if (const std::optional<AuditRecord> recovered = parseAuditLine(tail)) {
        warnOnce(directory, lineNumber, line.size(), "hole-recovered",
                 std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " lost " + std::to_string(hole) +
                     " bytes to a power cut — the " + std::to_string(tail.size()) +
                     " bytes after the hole are a whole record and are kept");
        records.push_back(*recovered);
      } else {
        warnOnce(directory, lineNumber, line.size(), "hole-torn",
                 std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is " + std::to_string(hole) +
                     " NUL bytes then " + std::to_string(tail.size()) +
                     " bytes that are not a record — a power cut tore this one, not a parse error");
      }
      continue;
    }
    if (const std::optional<AuditRecord> record = parseAuditLine(line)) {
      records.push_back(*record);
    } else if (lineNumber == lines.size()) {
      logInfo(kTag, "%s ends mid-record — something was killed while writing it", kAuditFile);
    } else {
      warnOnce(directory, lineNumber, line.size(), "damaged",
               std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is not valid JSON");
    }
  }
  std::reverse(records.begin(), records.end());
  if (records.size() > limit) records.resize(limit);
  return records;
}

}  // namespace emcool::vcu
