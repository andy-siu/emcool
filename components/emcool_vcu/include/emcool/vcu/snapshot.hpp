#pragma once

// One sweep's parameters as rows: each read outcome as a row (named, typed and valued from the
// table in use), the verdict on the bike's own TABLE_TYPE, re-naming a stored snapshot from the
// table the bike says it runs, and what changed between two snapshots. Rows are written and read
// back as JavaScript's JSON.stringify writes them.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/tables.hpp"

namespace emcool::vcu {

enum class ReadStatus : std::uint8_t {
  kRead,
  kRefused,
  kNoResponse,
  kNoSession,
  kStalled,
  kAbandoned,
  kUnrecognised,
  kNotSent,
};
inline constexpr ReadStatus kReadStatuses[] = {ReadStatus::kRead,      ReadStatus::kRefused,  ReadStatus::kNoResponse,
                                               ReadStatus::kNoSession, ReadStatus::kStalled,  ReadStatus::kAbandoned,
                                               ReadStatus::kUnrecognised, ReadStatus::kNotSent};
const char* readStatusName(ReadStatus status);
std::optional<ReadStatus> readStatusNamed(std::string_view name);

// How long after a first frame arrived our flow control went out.
struct ArrivalLatency {
  bool known = false;
  double ms = 0;
  std::string reason;
};
// A frame's arrival, if the driver stamped it, and now on the same clock.
ArrivalLatency arrivalLatencyMs(std::optional<double> arrivalMs, double nowMs);
void writeLatencyJson(text::TextBuffer& out, const std::optional<ArrivalLatency>& latency);

// How one read came out: what was asked, and what happened.
struct VcuReadResult {
  ReadStatus status = ReadStatus::kNotSent;
  std::vector<std::uint8_t> record;  // kRead
  int negativeResponseCode = 0;      // kRefused
  std::string description;           // kRefused
  std::string reason;                // kNoSession, kStalled, kAbandoned, kUnrecognised, kNotSent
  std::optional<ArrivalLatency> flowControlLatency;
};
struct VcuReadOutcome : VcuReadResult {
  Micro micro = Micro::kA9;
  int bank = kCalibrationBank;
  int index = 0;
  int identifier = 0;
};

struct VcuParameterRow {
  // A row resumed from the partial-sweep file carries that file's stamp, written first.
  std::optional<double> at;
  int index = 0;
  int identifier = 0;
  Micro micro = Micro::kA9;
  std::optional<std::string> name;
  std::optional<std::string> section;
  std::optional<StorageType> type;
  std::optional<bool> isSigned;
  ReadStatus status = ReadStatus::kNotSent;
  std::optional<double> otherBikeValue;
  std::optional<std::string> rawHex;
  std::optional<double> unsignedValue;
  std::optional<double> value;
  bool widthMismatch = false;
  std::optional<std::string> note;
};

// note(): the parts that say something, joined with " — ", or null.
std::optional<std::string> note(std::initializer_list<std::optional<std::string>> parts);

VcuParameterRow toParameterRow(const VcuReadOutcome& outcome, const ParameterNames& names);
// "  12 NAME…   value  [raw]" for the log.
std::string describeRow(const VcuParameterRow& row);

void writeRowJson(text::TextBuffer& out, const VcuParameterRow& row);
// A row as writeRowJson wrote it (or JavaScript's JSON.stringify would). nullopt if it isn't one.
std::optional<VcuParameterRow> parseRowJson(std::string_view json);
// "0A FF" back to bytes; nullopt (and a warning) if it isn't hex.
std::optional<std::vector<std::uint8_t>> bytesFromHex(std::string_view rawHex);

struct SnapshotHeader {
  double readAt = 0;
  bool complete = false;
  std::vector<Micro> micros;
};
void writeHeaderJson(text::TextBuffer& out, const SnapshotHeader& header);  // its members, no braces
std::optional<SnapshotHeader> parseHeaderJson(std::string_view json);

struct TableTypeReport {
  std::vector<TableTypeVerdict> verdicts;
  bool confirmed = false;
  std::optional<double> tableType;
  bool split = false;
  std::vector<int> unread;
  std::vector<int> unusable;
  bool mismatched = false;
  std::vector<std::string> lines;
};
// From the snapshot's rows at the TABLE_TYPE indices; `rowAt` returns null for a row not present.
TableTypeReport reportTableType(const std::function<const VcuParameterRow*(int index)>& rowAt,
                                const ParameterNames& names);
void writeReportJson(text::TextBuffer& out, const TableTypeReport& report);

// retableSnapshot()'s per-row step, with what it works out once per snapshot.
class Retabler {
 public:
  Retabler(const TableTypeReport& report, const ParameterNames& names);
  bool changesNothing() const { return byMicro_.empty(); }
  VcuParameterRow retable(const VcuParameterRow& row) const;

 private:
  // Per micro: the table its verdict named (null if uncarried); absent if it gave none.
  std::vector<std::pair<Micro, const VcuParameterTable*>> byMicro_;
  const VcuParameterTable* agreed_;
};

struct VcuParameterChange {
  enum class Kind : std::uint8_t { kValueChanged, kStatusChanged, kAppeared, kDisappeared };
  Kind kind;
  int index;
  std::optional<std::string> name;
  Micro micro;
  std::string fromRaw, toRaw;
  std::optional<double> fromValue, toValue;
  ReadStatus fromStatus = ReadStatus::kRead, toStatus = ReadStatus::kRead;
};
// diffSnapshots(), as a merge of two index-sorted row streams: each source hands out its next row,
// or nullopt at the end. Changes come out in index order.
using RowSource = std::function<std::optional<VcuParameterRow>()>;
void diffSnapshots(const RowSource& previous, const RowSource& current,
                   const std::function<void(const VcuParameterChange&)>& emit);
std::string describeChange(const VcuParameterChange& change);

}  // namespace emcool::vcu
