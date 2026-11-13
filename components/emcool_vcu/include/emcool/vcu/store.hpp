#pragma once

// Where service mode keeps its parameter snapshots, and the readers behind /vcu-params and
// /vcu-backup.
//
// A sweep is 302 rows of about 250 bytes, more than the ESP32 has to spare, so emcool keeps them
// on the card as JSON lines and never holds a whole snapshot:
//   sweep.partial.jsonl   every row as it's read, `{"at":…, …row}` — the resume file, and
//                         also the sweep's working record: the snapshot is written from it
//   <ISO time>.jsonl      each snapshot: a header line, then one row per line, as JavaScript's
//                         JSON.stringify writes a row. Never rewritten.
//   latest.txt            the name of the snapshot the dashboard is shown,
//                         replaced only by a sweep that read at least as much
// The dashboard's bodies are written out from those a row at a time.

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <vector>

#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/snapshot.hpp"

namespace emcool::vcu {

// What the sweep needs to know about a row without holding it.
struct RowSummary {
  Micro micro;
  ReadStatus status;
};

class VcuStore {
 public:
  VcuStore(std::string directory, ParameterNames& names) : directory_(std::move(directory)), names_(names) {}

  const std::string& directory() const { return directory_; }
  ParameterNames& names() { return names_; }

  // loadPartialRows(): what an earlier, unfinished sweep already read — and in full, the rows at the
  // TABLE_TYPE indices, which the end of the sweep reports on.
  std::map<int, RowSummary> loadPartialRows(std::map<int, VcuParameterRow>* tableTypeRows = nullptr);

  class PartialLog {
   public:
    explicit PartialLog(std::FILE* file, long startOffset) : file_(file), startOffset_(startOffset) {}
    PartialLog(PartialLog&& other) noexcept : file_(std::exchange(other.file_, nullptr)), startOffset_(other.startOffset_) {}
    ~PartialLog() { close(); }
    bool append(const VcuParameterRow& row, double wallClockMs);
    void close();
    // Where this sweep's rows begin in the file: the rows before were resumed.
    long startOffset() const { return startOffset_; }

   private:
    std::FILE* file_;
    long startOffset_;
  };
  std::optional<PartialLog> openPartialLog();

  // writeSnapshot(): the sweep's rows (the resume file's latest for each index) re-named from the
  // table the bike reports, written as a snapshot; latest moved to it if it read at least as much;
  // the resume file cleared if it's complete; and what changed since the last, logged.
  void writeSnapshot(const SnapshotHeader& header, long thisRunStartOffset);

  // At startup: the report on the latest snapshot's TABLE_TYPE rows, if there is one.
  std::optional<TableTypeReport> loadLatestTableType();

  // The latest snapshot's name, or nullopt; and whether reading it failed.
  struct Latest {
    enum class State : std::uint8_t { kNeverRead, kSnapshot, kUnreadable };
    State state;
    std::string path;
    std::string reason;
  };
  Latest latest();

  // /vcu-read's `export`: rows the backup would hold, and the latest snapshot's stamp.
  struct ExportSummary {
    int rows = 0;
    std::optional<double> readAt;
    bool complete = false;
  };
  ExportSummary exportSummary();

  // Guards latest.txt, which the diagnostics task replaces while the Bluetooth task reads it.
  std::mutex& latestMutex() { return latestMutex_; }

 private:
  std::string path(const char* name) const { return directory_ + "/" + name; }
  bool makeDirectory();
  void setLatest(const std::string& archiveName);

  std::string directory_;
  ParameterNames& names_;
  std::mutex latestMutex_;
  std::string summaryFor_;
  ExportSummary summary_;
};

// A snapshot file read back a row at a time: its header, then each row re-named as the dashboard
// is shown it.
class SnapshotReader {
 public:
  // Opens it, and works out the TABLE_TYPE report from its rows 276 and 277.
  bool open(const std::string& path, const ParameterNames& names);
  const SnapshotHeader& header() const { return header_; }
  const TableTypeReport& report() const { return report_; }
  // From the first row again.
  void rewind();
  std::optional<VcuParameterRow> next();  // re-named
  std::optional<VcuParameterRow> nextAsStored();
  ~SnapshotReader();

 private:
  std::FILE* file_ = nullptr;
  long rowsOffset_ = 0;
  SnapshotHeader header_;
  TableTypeReport report_;
  std::unique_ptr<Retabler> retabler_;
};

// One line at a time, up to `capacity`; the newline dropped. False at the end.
bool readLine(std::FILE* file, std::string& line);

// "2026-11-02T10-00-00.123Z": the snapshot's file name for `readAt`.
std::string isoStampForFile(double wallClockMs);

// The rows the CSV backup holds.
bool isExportable(const VcuParameterRow& row);
// One CSV line, \r\n included; the header line for a null row.
std::string backupCsvLine(const VcuParameterRow* row);
inline constexpr const char* kBackupCsvFilename = "vcu_backup.csv";

}  // namespace emcool::vcu
