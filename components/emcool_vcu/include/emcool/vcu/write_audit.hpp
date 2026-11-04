#pragma once

// The service-write audit journal: every write attempt appended to service-writes.jsonl, and the
// last few read back for the page. The recovery of records a power cut tore (leading NUL bytes)
// is handled too.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/text/text_buffer.hpp"

namespace emcool::vcu {

enum class AuditAction : std::uint8_t {
  kParameterWrite,
  kSetServicePoint,
  kClearDtcs,
  kRtcSync,
  kChargeCurrent,
  kChargeStop,
  kChargeSocLimit,
  kChargeSocLimitRead,
  kResetVcu,
  kReadServiceStamp,
};
const char* auditActionName(AuditAction action);

// A field that may be absent, null, a number or a string (`number | string | null`).
struct AuditValue {
  enum class Kind : std::uint8_t { kAbsent, kNull, kNumber, kString };
  Kind kind = Kind::kAbsent;
  double number = 0;
  std::string text;

  static AuditValue absent() { return {}; }
  static AuditValue null() { return {Kind::kNull, 0, {}}; }
  static AuditValue of(double value) { return {Kind::kNumber, value, {}}; }
  static AuditValue of(std::string value) { return {Kind::kString, 0, std::move(value)}; }
};

struct AuditRecord {
  double at = 0;
  bool clockTrustworthy = false;
  AuditAction action = AuditAction::kParameterWrite;
  std::string status;
  std::optional<std::string> name;
  std::optional<int> identifier;
  std::optional<std::string> micro;
  AuditValue before;
  AuditValue after;
  AuditValue requested;
  std::optional<std::string> rawHex;
  std::optional<std::string> note;
  std::optional<std::string> runningVersion;
};

void writeAuditRecordJson(text::TextBuffer& out, const AuditRecord& record);

// Appends one record, stamping `runningVersion`. Loud on failure; the action already happened.
void appendAuditRecord(const std::string& directory, AuditRecord record, const std::string& runningVersion);
// The last `limit` records, newest first. Tolerates power-cut holes.
std::vector<AuditRecord> recentAuditRecords(const std::string& directory, std::size_t limit);

}  // namespace emcool::vcu
