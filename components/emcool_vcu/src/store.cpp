#include "emcool/vcu/store.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

#include "emcool/log.hpp"
#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-sweep";
constexpr const char* kPartialFile = "sweep.partial.jsonl";
constexpr const char* kLatestFile = "latest.txt";

std::string rowLine(const VcuParameterRow& row) {
  std::string text(4096, '\0');
  text::TextBuffer out(text.data(), text.size());
  writeRowJson(out, row);
  text.resize(out.overflowed() ? 0 : out.length());
  return text;
}

// "2026-11-02T10:00:00.000Z", as Date.prototype.toISOString writes it.
std::string isoStamp(double wallClockMs) {
  const auto ms = static_cast<long long>(wallClockMs);
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm utc = {};
  gmtime_r(&seconds, &utc);
  char text[96];
  std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, ms % 1000);
  return text;
}

// A line and whether a newline ended it.
bool readSegment(std::FILE* file, std::string& line, bool& terminated) {
  line.clear();
  terminated = false;
  int character;
  bool any = false;
  while ((character = std::fgetc(file)) != EOF) {
    any = true;
    if (character == '\n') {
      terminated = true;
      return true;
    }
    line.push_back(static_cast<char>(character));
  }
  return any;
}

bool isBlank(const std::string& line) {
  return std::all_of(line.begin(), line.end(), [](char character) {
    return character == ' ' || character == '\t' || character == '\r';
  });
}

bool isAllNul(const std::string& line) {
  bool sawNul = false;
  for (const char character : line) {
    if (character == '\0') {
      sawNul = true;
    } else if (character != ' ' && character != '\t' && character != '\r') {
      return false;
    }
  }
  return sawNul;
}

}  // namespace

bool readLine(std::FILE* file, std::string& line) {
  bool terminated = false;
  return readSegment(file, line, terminated);
}

std::string isoStampForFile(double wallClockMs) {
  std::string stamp = isoStamp(wallClockMs);
  std::replace(stamp.begin(), stamp.end(), ':', '-');
  return stamp;
}

bool VcuStore::makeDirectory() {
  if (::mkdir(directory_.c_str(), 0775) == 0 || errno == EEXIST) return true;
  logWarning(kTag, "could not create %s: %s", directory_.c_str(), std::strerror(errno));
  return false;
}

std::map<int, RowSummary> VcuStore::loadPartialRows(std::map<int, VcuParameterRow>* tableTypeRows) {
  std::map<int, RowSummary> rows;
  const std::string file = path(kPartialFile);
  std::FILE* input = std::fopen(file.c_str(), "rb");
  if (input == nullptr) {
    if (errno != ENOENT) logWarning(kTag, "could not read %s — starting fresh: %s", file.c_str(), std::strerror(errno));
    return rows;
  }
  std::string line;
  bool terminated = false;
  int position = 0;
  while (readSegment(input, line, terminated)) {
    position++;
    if (isBlank(line)) continue;
    if (isAllNul(line)) {
      logWarning(kTag, "%s line %d is %zu NUL bytes — a row lost to a power cut, not a parse error", kPartialFile,
                 position, line.size());
      continue;
    }
    const std::optional<VcuParameterRow> row = parseRowJson(line);
    if (!row) {
      if (!terminated) {
        logInfo(kTag, "%s ends mid-row — a sweep was killed while writing it; re-reading that one", kPartialFile);
      } else {
        logWarning(kTag, "%s line %d is not valid JSON, skipping it", kPartialFile, position);
      }
      continue;
    }
    rows[row->index] = {row->micro, row->status};
    if (tableTypeRows != nullptr && isTableTypeIndex(row->index)) tableTypeRows->insert_or_assign(row->index, *row);
  }
  std::fclose(input);
  return rows;
}

std::optional<VcuStore::PartialLog> VcuStore::openPartialLog() {
  if (!makeDirectory()) return std::nullopt;
  std::FILE* file = std::fopen(path(kPartialFile).c_str(), "ab");
  if (file == nullptr) {
    logWarning(kTag, "could not open %s: %s", path(kPartialFile).c_str(), std::strerror(errno));
    return std::nullopt;
  }
  std::fseek(file, 0, SEEK_END);
  return PartialLog(file, std::ftell(file));
}

bool VcuStore::PartialLog::append(const VcuParameterRow& row, double wallClockMs) {
  if (file_ == nullptr) return false;
  VcuParameterRow stamped = row;
  stamped.at = wallClockMs;
  const std::string line = rowLine(stamped) + "\n";
  const bool written = std::fwrite(line.data(), 1, line.size(), file_) == line.size() && std::fflush(file_) == 0;
  if (!written) logWarning(kTag, "could not append to %s: %s", kPartialFile, std::strerror(errno));
  return written;
}

void VcuStore::PartialLog::close() {
  if (file_ == nullptr) return;
  std::fclose(file_);
  file_ = nullptr;
}

void VcuStore::setLatest(const std::string& archiveName) {
  const std::lock_guard<std::mutex> lock(latestMutex_);
  const std::string temporary = path("latest.tmp");
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    logWarning(kTag, "could not write %s: %s", temporary.c_str(), std::strerror(errno));
    return;
  }
  std::fputs(archiveName.c_str(), file);
  std::fflush(file);
  std::fclose(file);
  // FAT can't rename over a file, so the old one goes first; a reader finding neither uses latest.tmp.
  std::remove(path(kLatestFile).c_str());
  if (std::rename(temporary.c_str(), path(kLatestFile).c_str()) != 0) {
    logWarning(kTag, "could not move %s into place: %s", temporary.c_str(), std::strerror(errno));
  }
}

VcuStore::Latest VcuStore::latest() {
  const std::lock_guard<std::mutex> lock(latestMutex_);
  for (const char* name : {kLatestFile, "latest.tmp"}) {
    std::FILE* file = std::fopen(path(name).c_str(), "rb");
    if (file == nullptr) continue;
    std::string archive;
    readLine(file, archive);
    std::fclose(file);
    if (archive.empty()) return {Latest::State::kUnreadable, {}, std::string(name) + " is empty"};
    return {Latest::State::kSnapshot, path(archive.c_str()), {}};
  }
  return {Latest::State::kNeverRead, {}, {}};
}

std::optional<TableTypeReport> VcuStore::loadLatestTableType() {
  const Latest stored = latest();
  if (stored.state != Latest::State::kSnapshot) return std::nullopt;
  SnapshotReader reader;
  if (!reader.open(stored.path, names_)) return std::nullopt;
  return reader.report();
}

VcuStore::ExportSummary VcuStore::exportSummary() {
  const Latest stored = latest();
  if (stored.state != Latest::State::kSnapshot) return {};
  if (stored.path == summaryFor_) return summary_;
  SnapshotReader reader;
  if (!reader.open(stored.path, names_)) return {};
  ExportSummary summary{0, reader.header().readAt, reader.header().complete};
  while (const std::optional<VcuParameterRow> row = reader.next()) {
    if (isExportable(*row)) summary.rows++;
  }
  summaryFor_ = stored.path;
  summary_ = summary;
  return summary;
}

void VcuStore::writeSnapshot(const SnapshotHeader& header, long thisRunStartOffset) {
  if (!makeDirectory()) return;
  // Each index's last row in the resume file, and where it is.
  std::map<int, long> offsets;
  {
    std::FILE* partial = std::fopen(path(kPartialFile).c_str(), "rb");
    if (partial != nullptr) {
      std::string line;
      long offset = std::ftell(partial);
      while (readLine(partial, line)) {
        if (const std::optional<VcuParameterRow> row = parseRowJson(line)) offsets[row->index] = offset;
        offset = std::ftell(partial);
      }
      std::fclose(partial);
    }
  }
  std::FILE* partial = std::fopen(path(kPartialFile).c_str(), "rb");
  const auto rowAtOffset = [&](long offset) -> std::optional<VcuParameterRow> {
    if (partial == nullptr) return std::nullopt;
    std::fseek(partial, offset, SEEK_SET);
    std::string line;
    if (!readLine(partial, line)) return std::nullopt;
    std::optional<VcuParameterRow> row = parseRowJson(line);
    // Read this run, it was the sweep's own row, with no stamp; resumed, it keeps the file's.
    if (row && offset >= thisRunStartOffset) row->at.reset();
    return row;
  };

  // The TABLE_TYPE report, from rows 276 and 277.
  std::map<int, VcuParameterRow> tableTypeRows;
  for (const int index : kTableTypeIndices) {
    const auto found = offsets.find(index);
    if (found == offsets.end()) continue;
    if (std::optional<VcuParameterRow> row = rowAtOffset(found->second)) tableTypeRows.emplace(index, std::move(*row));
  }
  const TableTypeReport report = reportTableType(
      [&](int index) -> const VcuParameterRow* {
        const auto found = tableTypeRows.find(index);
        return found == tableTypeRows.end() ? nullptr : &found->second;
      },
      names_);
  const Retabler retabler(report, names_);
  if (report.tableType) names_.select(static_cast<int>(*report.tableType));

  const Latest baseline = latest();
  const std::string archiveName = isoStampForFile(header.readAt) + ".jsonl";
  const std::string archivePath = path(archiveName.c_str());
  int read = 0;
  int total = 0;
  {
    std::FILE* archive = std::fopen(archivePath.c_str(), "wb");
    if (archive == nullptr) {
      logError(kTag, "could not write %s: %s", archivePath.c_str(), std::strerror(errno));
      if (partial != nullptr) std::fclose(partial);
      return;
    }
    std::string headerText(256, '\0');
    text::TextBuffer out(headerText.data(), headerText.size());
    out.append("{");
    writeHeaderJson(out, header);
    out.append("}\n");
    std::fwrite(headerText.data(), 1, out.length(), archive);
    for (const auto& [index, offset] : offsets) {
      std::optional<VcuParameterRow> row = rowAtOffset(offset);
      if (!row) continue;
      const VcuParameterRow retabled = retabler.retable(*row);
      if (retabled.status == ReadStatus::kRead) read++;
      total++;
      const std::string line = rowLine(retabled) + "\n";
      std::fwrite(line.data(), 1, line.size(), archive);
    }
    std::fflush(archive);
    std::fclose(archive);
  }
  if (partial != nullptr) std::fclose(partial);

  int baselineRead = 0;
  if (baseline.state == Latest::State::kSnapshot) {
    SnapshotReader before;
    if (before.open(baseline.path, names_)) {
      while (const std::optional<VcuParameterRow> row = before.nextAsStored()) {
        if (row->status == ReadStatus::kRead) baselineRead++;
      }
    }
  }
  const bool replacesLatest = read > 0 && (header.complete || read >= baselineRead);
  if (replacesLatest) setLatest(archiveName);
  const std::string incomplete = header.complete ? "" : "  ⚠️ INCOMPLETE — start it again to resume";
  if (replacesLatest) {
    logInfo(kTag, "%d/%d read%s · wrote %s and %s", read, total, incomplete.c_str(), archivePath.c_str(), kLatestFile);
  } else if (read > 0) {
    logInfo(kTag, "%d/%d read%s · wrote %s; KEPT the previous %s — it holds %d read values against this run's %d", read,
            total, incomplete.c_str(), archivePath.c_str(), kLatestFile, baselineRead, read);
  } else {
    logInfo(kTag, "%d/%d read%s · wrote %s; left %s as it was — nothing was read, so the baseline stands", read, total,
            incomplete.c_str(), archivePath.c_str(), kLatestFile);
  }
  if (header.complete && std::remove(path(kPartialFile).c_str()) != 0 && errno != ENOENT) {
    logWarning(kTag, "could not clear %s; a resume file may outlive this sweep: %s", kPartialFile, std::strerror(errno));
  }

  // What changed against the snapshot before.
  if (baseline.state != Latest::State::kSnapshot) {
    logInfo(kTag, "no previous snapshot to compare against — this one becomes the baseline");
    return;
  }
  SnapshotReader before;
  SnapshotReader after;
  if (!before.open(baseline.path, names_) || !after.open(archivePath, names_)) {
    logInfo(kTag, "no previous snapshot to compare against — this one becomes the baseline");
    return;
  }
  std::vector<std::string> lines;
  int valueChanges = 0;
  diffSnapshots([&] { return before.nextAsStored(); }, [&] { return after.nextAsStored(); },
                [&](const VcuParameterChange& change) {
                  lines.push_back(describeChange(change));
                  if (change.kind == VcuParameterChange::Kind::kValueChanged) valueChanges++;
                });
  const std::string when = isoStamp(before.header().readAt);
  if (lines.empty()) {
    logInfo(kTag, "no differences at all against %s", when.c_str());
    return;
  }
  logInfo(kTag, "compared against %s:", when.c_str());
  for (const std::string& line : lines) logInfo(kTag, "  %s", line.c_str());
  if (valueChanges > 0) {
    logWarning(kTag, "⚠️  %d PARAMETER VALUE(S) CHANGED — something reconfigured the bike.", valueChanges);
  }
}

bool SnapshotReader::open(const std::string& path, const ParameterNames& names) {
  file_ = std::fopen(path.c_str(), "rb");
  if (file_ == nullptr) return false;
  std::string line;
  if (!readLine(file_, line)) return false;
  const std::optional<SnapshotHeader> header = parseHeaderJson(line);
  if (!header) return false;
  header_ = *header;
  rowsOffset_ = std::ftell(file_);
  std::map<int, VcuParameterRow> tableTypeRows;
  while (const std::optional<VcuParameterRow> row = nextAsStored()) {
    if (isTableTypeIndex(row->index)) tableTypeRows.insert_or_assign(row->index, *row);
  }
  report_ = reportTableType(
      [&](int index) -> const VcuParameterRow* {
        const auto found = tableTypeRows.find(index);
        return found == tableTypeRows.end() ? nullptr : &found->second;
      },
      names);
  retabler_ = std::make_unique<Retabler>(report_, names);
  rewind();
  return true;
}

void SnapshotReader::rewind() {
  if (file_ != nullptr) std::fseek(file_, rowsOffset_, SEEK_SET);
}

std::optional<VcuParameterRow> SnapshotReader::nextAsStored() {
  std::string line;
  while (file_ != nullptr && readLine(file_, line)) {
    if (isBlank(line)) continue;
    if (std::optional<VcuParameterRow> row = parseRowJson(line)) return row;
  }
  return std::nullopt;
}

std::optional<VcuParameterRow> SnapshotReader::next() {
  std::optional<VcuParameterRow> row = nextAsStored();
  if (!row) return std::nullopt;
  return retabler_->retable(*row);
}

SnapshotReader::~SnapshotReader() {
  if (file_ != nullptr) std::fclose(file_);
}

bool isExportable(const VcuParameterRow& row) {
  return row.status == ReadStatus::kRead && row.value.has_value() && row.name.has_value();
}

std::string backupCsvLine(const VcuParameterRow* row) {
  if (row == nullptr) return "id_hex,name,value\r\n";
  char identifier[16];
  std::snprintf(identifier, sizeof identifier, "0x%02X", row->index);
  std::string name = row->name.value_or("");
  if (name.find_first_of("\",\r\n") != std::string::npos) {
    std::string quoted = "\"";
    for (const char character : name) {
      quoted += character;
      if (character == '"') quoted += '"';
    }
    name = quoted + "\"";
  }
  return std::string(identifier) + "," + name + "," + jsNumber(*row->value) + "\r\n";
}

}  // namespace emcool::vcu
