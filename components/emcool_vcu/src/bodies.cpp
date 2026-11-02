#include "emcool/vcu/bodies.hpp"

namespace emcool::vcu {

namespace {

constexpr const char* kNeverReadHint =
    "start one from the dashboard: Menu → Service mode, with the bike awake, parked and out of drive";

template <typename Write>
std::string build(Write write) {
  std::string text(8192, '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.overflowed() ? 0 : out.length());
  return text;
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

void writeReadResponseJson(text::TextBuffer& out, const VcuReadRunner& runner, VcuStore& store, bool enabled,
                           const std::optional<std::string>& message) {
  out.append("{\"run\":");
  runner.writeStateJson(out);
  out.append(",\"gate\":");
  writeVerdictJson(out, runner.gate());
  out.append(enabled ? ",\"enabled\":true,\"export\":{\"rows\":" : ",\"enabled\":false,\"export\":{\"rows\":");
  const VcuStore::ExportSummary summary = store.exportSummary();
  out.appendInteger(summary.rows);
  out.append(",\"readAt\":");
  if (summary.readAt) {
    out.appendNumber(*summary.readAt);
  } else {
    out.append("null");
  }
  out.append(summary.complete ? ",\"complete\":true},\"message\":" : ",\"complete\":false},\"message\":");
  writeOptionalString(out, message);
  out.append("}");
}

void writeProbeResponseJson(text::TextBuffer& out, const VcuProbeReading* reading,
                            const std::optional<std::string>& message, const ServiceGateVerdict& gate) {
  out.append("{\"reading\":");
  if (reading != nullptr) {
    writeProbeReadingJson(out, *reading);
  } else {
    out.append("null");
  }
  out.append(",\"message\":");
  writeOptionalString(out, message);
  out.append(",\"gate\":");
  writeVerdictJson(out, gate);
  out.append(",\"targets\":[\"A9\",\"A8\"]}");
}

void writeStartedJson(text::TextBuffer& out, const VcuReadRunner::Started& started) {
  out.append(started.started ? "{\"started\":true,\"reason\":" : "{\"started\":false,\"reason\":");
  writeOptionalString(out, started.reason);
  out.append("}");
}

void writeProbeOutcomeJson(text::TextBuffer& out, const ProbeOutcome& outcome) {
  if (outcome.reading) {
    out.append("{\"ok\":true,\"reading\":");
    writeProbeReadingJson(out, *outcome.reading);
  } else {
    out.append("{\"ok\":false,\"reason\":");
    out.appendJsonString(outcome.reason);
  }
  out.append("}");
}

// --- /vcu-params --------------------------------------------------------------------------------

ParamsBody::ParamsBody(VcuStore& store, std::string displayDirectory)
    : latest_(store.latest()), displayDirectory_(std::move(displayDirectory)) {
  if (latest_.state == VcuStore::Latest::State::kSnapshot) {
    reader_ = std::make_unique<SnapshotReader>();
    if (!reader_->open(latest_.path, store.names())) {
      latest_ = {VcuStore::Latest::State::kUnreadable, latest_.path, "the latest snapshot is not a parameter snapshot"};
      reader_.reset();
    }
  }
  if (latest_.state == VcuStore::Latest::State::kNeverRead) {
    whole_ = build([&](text::TextBuffer& out) {
      out.append("{\"state\":\"never-read\",\"directory\":");
      out.appendJsonString(displayDirectory_);
      out.append(",\"hint\":");
      out.appendJsonString(kNeverReadHint);
      out.append("}");
    });
  } else if (latest_.state == VcuStore::Latest::State::kUnreadable) {
    whole_ = build([&](text::TextBuffer& out) {
      out.append("{\"state\":\"unreadable\",\"directory\":");
      out.appendJsonString(displayDirectory_);
      out.append(",\"reason\":");
      out.appendJsonString(latest_.reason);
      out.append("}");
    });
  }
}

void ParamsBody::rewind() {
  stage_ = Stage::kOpening;
  firstRow_ = true;
  if (reader_) reader_->rewind();
}

std::optional<std::string> ParamsBody::next() {
  if (!reader_) {
    if (stage_ == Stage::kDone) return std::nullopt;
    stage_ = Stage::kDone;
    return whole_;
  }
  switch (stage_) {
    case Stage::kOpening:
      stage_ = Stage::kRows;
      return build([&](text::TextBuffer& out) {
        out.append("{\"state\":\"snapshot\",\"snapshot\":{");
        writeHeaderJson(out, reader_->header());
        out.append(",\"rows\":[");
      });
    case Stage::kRows:
      if (const std::optional<VcuParameterRow> row = reader_->next()) {
        std::string part = build([&](text::TextBuffer& out) { writeRowJson(out, *row); });
        if (!firstRow_) part.insert(part.begin(), ',');
        firstRow_ = false;
        return part;
      }
      stage_ = Stage::kClosing;
      [[fallthrough]];
    case Stage::kClosing:
      stage_ = Stage::kDone;
      return build([&](text::TextBuffer& out) {
        out.append("]},\"tableType\":");
        writeReportJson(out, reader_->report());
        out.append("}");
      });
    case Stage::kDone:
      return std::nullopt;
  }
  return std::nullopt;
}

// --- /vcu-backup --------------------------------------------------------------------------------

BackupBody::BackupBody(VcuStore& store, std::string displayDirectory) {
  const VcuStore::Latest latest = store.latest();
  if (latest.state == VcuStore::Latest::State::kNeverRead) {
    status_ = 404;
    message_ = "no parameter snapshot in " + displayDirectory + " yet — " + kNeverReadHint + "\n";
    return;
  }
  reader_ = std::make_unique<SnapshotReader>();
  if (latest.state == VcuStore::Latest::State::kUnreadable || !reader_->open(latest.path, store.names())) {
    status_ = 500;
    message_ = "the snapshot in " + displayDirectory + " could not be read: " +
               (latest.reason.empty() ? std::string("it is not a parameter snapshot") : latest.reason) + "\n";
    reader_.reset();
    return;
  }
  while (const std::optional<VcuParameterRow> row = reader_->next()) {
    if (isExportable(*row)) rows_++;
  }
  reader_->rewind();
  if (rows_ == 0) {
    status_ = 409;
    message_ = "the last snapshot holds no readable values — re-run the read with the bike awake\n";
    reader_.reset();
  }
}

const char* BackupBody::contentType() const {
  return status_ == 200 ? "text/csv; charset=utf-8" : "text/plain; charset=utf-8";
}

void BackupBody::rewind() {
  headerSent_ = false;
  done_ = false;
  if (reader_) reader_->rewind();
}

std::optional<std::string> BackupBody::next() {
  if (done_) return std::nullopt;
  if (!reader_) {
    done_ = true;
    return message_;
  }
  if (!headerSent_) {
    headerSent_ = true;
    return backupCsvLine(nullptr);
  }
  // The snapshot's rows are in index order already, which is the CSV's.
  while (const std::optional<VcuParameterRow> row = reader_->next()) {
    if (isExportable(*row)) return backupCsvLine(&*row);
  }
  done_ = true;
  return std::nullopt;
}

std::string wholeBody(PartsBody& body) {
  std::string text;
  body.rewind();
  while (const std::optional<std::string> part = body.next()) text += *part;
  return text;
}

}  // namespace emcool::vcu
