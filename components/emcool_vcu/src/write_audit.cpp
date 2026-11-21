#include "emcool/vcu/write_audit.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <span>
#include <utility>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/text/big_text.hpp"

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
  // In PSRAM: a line is as long as whatever a power cut left in it.
  text::BigText scratch = text::BigText::uninitialised(line.size() + 16);
  if (scratch.failed() || !reader.read(line, std::span<char>(scratch.data(), scratch.size()))) return std::nullopt;
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

namespace {

// The journal a line at a time, each line in PSRAM: the file grows with every write, a charge's
// automatic commands included, and a power cut can leave a cluster's worth of NUL bytes in one line.
class JournalLines {
 public:
  explicit JournalLines(std::FILE* file) : file_(file) {
    const long at = std::ftell(file);
    chunkStart_ = at < 0 ? 0 : at;
  }

  // The next line without its newline, where in the file it starts, and whether it is the file's
  // last; false at the end, or if there's no memory for the line.
  bool next(std::string_view& line, long& start, bool& last) {
    if (finished_) return false;
    start = chunkStart_ + static_cast<long>(at_);
    length_ = 0;
    bool any = false;
    while (true) {
      if (at_ == filled_ && !refill()) {
        // The end, with or without a line before it: an unterminated line is the last one.
        finished_ = true;
        if (!any) return false;
        last = true;
        line = std::string_view(line_.data(), length_);
        return true;
      }
      any = true;
      const char* from = chunk_ + at_;
      const void* newline = std::memchr(from, '\n', filled_ - at_);
      const std::size_t take =
          newline == nullptr ? filled_ - at_ : static_cast<std::size_t>(static_cast<const char*>(newline) - from);
      if (!append(from, take)) {
        finished_ = true;
        return false;
      }
      at_ += take;
      if (newline != nullptr) {
        at_++;
        // A newline ends the file's last line when nothing comes after it.
        last = at_ == filled_ && !refill();
        if (last) finished_ = true;
        line = std::string_view(line_.data(), length_);
        return true;
      }
    }
  }

 private:
  bool refill() {
    chunkStart_ += static_cast<long>(filled_);
    at_ = 0;
    filled_ = std::fread(chunk_, 1, sizeof chunk_, file_);
    return filled_ > 0;
  }

  bool append(const char* bytes, std::size_t count) {
    if (length_ + count > line_.size()) {
      text::BigText bigger = text::BigText::uninitialised(std::max<std::size_t>(line_.size() * 2, length_ + count + 256));
      if (bigger.failed()) return false;
      if (length_ > 0) std::memcpy(bigger.data(), line_.data(), length_);
      line_ = std::move(bigger);
    }
    if (count > 0) std::memcpy(line_.data() + length_, bytes, count);
    length_ += count;
    return true;
  }

  std::FILE* file_;
  char chunk_[512];
  long chunkStart_ = 0;
  std::size_t at_ = 0;
  std::size_t filled_ = 0;
  bool finished_ = false;
  text::BigText line_;
  std::size_t length_ = 0;
};

}  // namespace

void forEachRecentAuditRecord(const std::string& directory, std::size_t limit,
                              const std::function<void(const AuditRecord&)>& visit) {
  if (limit == 0) return;
  const std::string path = directory + "/" + kAuditFile;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return;

  // First where the newest `limit` records start, however long the journal is, then each one read
  // again from there, newest first: only one record is ever held.
  struct Found {
    long start;
    std::size_t hole;  // the NUL bytes a power cut left in front of it
  };
  std::vector<Found> newest(limit);
  std::size_t found = 0;
  {
    JournalLines lines(file);
    std::string_view line;
    long start = 0;
    bool last = false;
    std::size_t lineNumber = 0;
    const auto keep = [&](std::size_t hole) { newest[found++ % limit] = {start, hole}; };
    while (lines.next(line, start, last)) {
      lineNumber++;
      if (isBlankOrNul(line)) {
        if (!line.empty() && line.find_first_not_of('\0') == std::string_view::npos) {
          warnOnce(directory, lineNumber, line.size(), "hole",
                   std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is " +
                       std::to_string(line.size()) + " NUL bytes — a record lost to a power cut, not a parse error");
        }
        continue;
      }
      const std::size_t hole = leadingNulBytes(line);
      if (hole > 0) {
        const std::string_view tail = line.substr(hole);
        if (parseAuditLine(tail)) {
          warnOnce(directory, lineNumber, line.size(), "hole-recovered",
                   std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " lost " + std::to_string(hole) +
                       " bytes to a power cut — the " + std::to_string(tail.size()) +
                       " bytes after the hole are a whole record and are kept");
          keep(hole);
        } else {
          warnOnce(directory, lineNumber, line.size(), "hole-torn",
                   std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is " + std::to_string(hole) +
                       " NUL bytes then " + std::to_string(tail.size()) +
                       " bytes that are not a record — a power cut tore this one, not a parse error");
        }
        continue;
      }
      if (parseAuditLine(line)) {
        keep(0);
      } else if (last) {
        logInfo(kTag, "%s ends mid-record — something was killed while writing it", kAuditFile);
      } else {
        warnOnce(directory, lineNumber, line.size(), "damaged",
                 std::string(kAuditFile) + " line " + std::to_string(lineNumber) + " is not valid JSON");
      }
    }
  }
  const std::size_t kept = std::min(found, limit);
  for (std::size_t back = 1; back <= kept; back++) {
    const Found& at = newest[(found - back) % limit];
    if (std::fseek(file, at.start, SEEK_SET) != 0) break;
    JournalLines lines(file);
    std::string_view line;
    long start = 0;
    bool last = false;
    if (!lines.next(line, start, last)) break;
    // Parsed the first time; only a journal changed in between would fail now.
    if (const std::optional<AuditRecord> record = parseAuditLine(line.substr(std::min(at.hole, line.size())))) {
      visit(*record);
    }
  }
  std::fclose(file);
}

}  // namespace emcool::vcu
