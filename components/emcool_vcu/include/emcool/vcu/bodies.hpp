#pragma once

// The bodies of the service-mode endpoints (/vcu-read, /vcu-probe, /vcu-params, /vcu-backup), as
// JSON.stringify writes them. The two that carry a snapshot are handed out a part at a time — the
// opening, then a row each, then the close — so a body of 100 KB never sits in memory.

#include <memory>
#include <optional>
#include <string>

#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/runner.hpp"
#include "emcool/vcu/store.hpp"

namespace emcool::vcu {

inline constexpr const char* kServiceModeHeader = "x-emcool";
inline constexpr const char* kServiceModeHeaderValue = "service-mode";

// GET/POST/DELETE /vcu-read's body: {run, gate, enabled, export, message}.
void writeReadResponseJson(text::TextBuffer& out, const VcuReadRunner& runner, VcuStore& store, bool enabled,
                           const std::optional<std::string>& message);
// /vcu-probe's: {reading, message, gate, targets}.
void writeProbeResponseJson(text::TextBuffer& out, const VcuProbeReading* reading,
                            const std::optional<std::string>& message, const ServiceGateVerdict& gate);
// runner.start()'s answer, as the handler sees it: {"started":…,"reason":…}.
void writeStartedJson(text::TextBuffer& out, const VcuReadRunner::Started& started);
void writeProbeOutcomeJson(text::TextBuffer& out, const ProbeOutcome& outcome);

// A body made of parts, read in order.
class PartsBody {
 public:
  virtual ~PartsBody() = default;
  // The next part, or nullopt at the end.
  virtual std::optional<std::string> next() = 0;
  // From the start again.
  virtual void rewind() = 0;
};

// /vcu-params: loadLatestSnapshot(), as {state, …}.
class ParamsBody final : public PartsBody {
 public:
  // `displayDirectory` is what the body says the directory is.
  ParamsBody(VcuStore& store, std::string displayDirectory);
  std::optional<std::string> next() override;
  void rewind() override;

 private:
  enum class Stage : std::uint8_t { kOpening, kRows, kClosing, kDone };
  VcuStore::Latest latest_;
  std::string displayDirectory_;
  std::unique_ptr<SnapshotReader> reader_;
  Stage stage_ = Stage::kOpening;
  bool firstRow_ = true;
  std::string whole_;  // a body that isn't a snapshot is one part
};

// /vcu-backup: the CSV, or why not, with the status to send it with.
class BackupBody final : public PartsBody {
 public:
  BackupBody(VcuStore& store, std::string displayDirectory);
  int status() const { return status_; }
  const char* contentType() const;
  int exportableRows() const { return rows_; }
  std::optional<std::string> next() override;
  void rewind() override;

 private:
  int status_ = 200;
  int rows_ = 0;
  std::string message_;
  std::unique_ptr<SnapshotReader> reader_;
  bool headerSent_ = false;
  bool done_ = false;
};

// All of a parts body, for a check.
std::string wholeBody(PartsBody& body);

}  // namespace emcool::vcu
