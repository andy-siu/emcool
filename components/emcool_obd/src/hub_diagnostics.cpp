#include "emcool/obd/hub_diagnostics.hpp"

#include <algorithm>
#include <cstdio>

#include "emcool/log.hpp"

namespace emcool::obd {

namespace {

constexpr const char* kTag = "diagnostics";
constexpr const char* kTransport = "can 0x410";
constexpr int kDiagnosticsMessageType = 25;
constexpr int kDiagnosticsInfoMessageType = 31;
constexpr int kFirstPage = 0x00;
constexpr int kLastPage = 0xfe;
constexpr int kWholeList = 0xff;
constexpr int kMaxPages = 100;
constexpr std::size_t kSeenSignalKeyLimit = 256;
constexpr int kRawFrameLogLimit = 24;
constexpr std::size_t kSideChannelPayloadLimit = 24;

std::string spacedHex(std::span<const std::uint8_t> frame) {
  std::string text;
  char pair[4];
  for (const std::uint8_t byte : frame) {
    if (!text.empty()) text += ' ';
    std::snprintf(pair, sizeof pair, "%02x", byte);
    text += pair;
  }
  return text;
}

// dtcSignalKey(): "dtc_0044_0".
std::string signalKey(int component, int symptom) {
  char text[24];
  std::snprintf(text, sizeof text, "dtc_%04d_%d", component, symptom);
  return text;
}

std::string diagnosticSignalKey(const DiagnosticCode& code) {
  if (code.entry != nullptr) return signalKey(code.entry->component, code.entry->symptom);
  return signalKey(code.component, code.symptom);
}

}  // namespace

bool isDiagnosticsMessage(std::span<const std::uint8_t> frame) {
  return frame.size() >= 8 && frame[0] == kDiagnosticsMessageType;
}

bool isDiagnosticsInfoMessage(std::span<const std::uint8_t> frame) {
  return frame.size() >= 8 && frame[0] == kDiagnosticsInfoMessageType;
}

DiagnosticCode describeDiagnosticCode(std::uint32_t raw, int flags) {
  const int component = static_cast<int>(raw & 0xffff);
  const int symptom = static_cast<int>((raw >> 16) & 0x0f);
  const ObdCodeText text = formatObdDtc(static_cast<std::uint16_t>(component));
  DiagnosticCode code{raw, component, symptom, std::string(text.view()), flags, nullptr, DiagnosticCode::MatchedBy::kNone};
  if (const DtcTableEntry* entry = lookupByComponentSymptom(component, symptom)) {
    code.entry = entry;
    code.matchedBy = DiagnosticCode::MatchedBy::kComponent;
  } else if (const DtcTableEntry* byObd = lookupByObdCode(text.view())) {
    code.entry = byObd;
    code.matchedBy = DiagnosticCode::MatchedBy::kObd;
  }
  return code;
}

std::string formatDiagnosticCode(const DiagnosticCode& code) {
  char raw[48];
  if (code.flags != 0) {
    std::snprintf(raw, sizeof raw, "raw 0x%05x flags 0x%x", static_cast<unsigned>(code.raw), code.flags);
  } else {
    std::snprintf(raw, sizeof raw, "raw 0x%05x", static_cast<unsigned>(code.raw));
  }
  const auto describe = [](const DtcTableEntry& entry) {
    return std::string(entry.obdCode) + " — " + std::string(entry.description);
  };
  if (code.entry != nullptr && code.matchedBy == DiagnosticCode::MatchedBy::kComponent) {
    return std::string(raw) + " · component " + std::to_string(code.component) + " symptom " +
           std::to_string(code.symptom) + " · " + describe(*code.entry);
  }
  if (code.entry != nullptr && code.matchedBy == DiagnosticCode::MatchedBy::kObd) {
    return std::string(raw) + " · read as OBD code · component " + std::to_string(code.entry->component) + " symptom " +
           std::to_string(code.entry->symptom) + " · " + describe(*code.entry);
  }
  return std::string(raw) + " · unrecognised (component " + std::to_string(code.component) + " symptom " +
         std::to_string(code.symptom) + ", or OBD " + code.obdCodeFromRaw + ")";
}

void DiagnosticListAssembler::reset() {
  codes_.clear();
  pages_ = 0;
}

std::optional<DiagnosticReport> DiagnosticListAssembler::push(std::span<const std::uint8_t> frame) {
  if (frame.size() < 8 || frame[0] != kDiagnosticsMessageType) return std::nullopt;
  const int subIndex = frame[1];
  if (subIndex == kFirstPage || subIndex == kWholeList) reset();
  pages_ += 1;
  for (const std::size_t offset : {std::size_t{2}, std::size_t{5}}) {
    const std::uint32_t value = frame[offset] | (frame[offset + 1] << 8) | ((frame[offset + 2] & 0x0f) << 16);
    const int flags = (frame[offset + 2] >> 4) & 0x0f;
    if (value == 0) continue;
    DiagnosticCode code = describeDiagnosticCode(value, flags);
    const bool known = std::any_of(codes_.begin(), codes_.end(),
                                   [&](const DiagnosticCode& existing) { return existing.raw == code.raw; });
    if (!known) codes_.push_back(code);
  }
  if (subIndex == kLastPage || subIndex == kWholeList || pages_ >= kMaxPages) {
    DiagnosticReport report{codes_, pages_};
    reset();
    return report;
  }
  return std::nullopt;
}

void HubDiagnostics::handle(std::span<const std::uint8_t> frame, SignalRecorder& recorder) {
  if (isDiagnosticsInfoMessage(frame)) {
    logSideChannel(frame);
    return;
  }
  if (!isDiagnosticsMessage(frame)) return;
  logRawFrame(frame);
  if (const std::optional<DiagnosticReport> report = assembler_.push(frame)) recordReport(*report, recorder);
}

void HubDiagnostics::recordReport(const DiagnosticReport& report, SignalRecorder& recorder) {
  // The keys present, in first-seen order (a Set, iterated in insertion order).
  std::vector<std::string> present;
  for (const DiagnosticCode& code : report.codes) {
    const std::string key = diagnosticSignalKey(code);
    if (std::find(present.begin(), present.end(), key) == present.end()) present.push_back(key);
  }
  recorder.record("dtc_list_count", static_cast<double>(report.codes.size()));
  recorder.record("dtc_unrecognised_count",
                  static_cast<double>(std::count_if(report.codes.begin(), report.codes.end(),
                                                    [](const DiagnosticCode& code) { return code.entry == nullptr; })));
  for (const std::string& key : present) {
    if (seenSignalKeys_.size() < kSeenSignalKeyLimit) {
      if (seenSignalKeys_.insert(key).second) seenOrder_.push_back(key);
    } else if (seenSignalKeys_.count(key) == 0) {
      if (!warnedSeenKeyLimit_) {
        warnedSeenKeyLimit_ = true;
        logWarning(kTag,
                   "more than %zu distinct code keys this run — the 20-bit code field is almost certainly not "
                   "(component, symptom). Codes past this point are still named in the journal below but are no "
                   "longer recorded as signals.",
                   kSeenSignalKeyLimit);
      }
      continue;
    }
    recorder.record(key, 1);
  }
  for (const std::string& key : seenOrder_) {
    if (std::find(present.begin(), present.end(), key) == present.end()) recorder.record(key, 0);
  }
  std::string signature;
  for (const DiagnosticCode& code : report.codes) {
    if (!signature.empty()) signature += ",";
    signature += std::to_string(code.raw);
  }
  if (lastSignature_ == signature) return;
  lastSignature_ = signature;
  logInfo(kTag, "%zu code(s) over %d page(s) via %s", report.codes.size(), report.pages, kTransport);
  for (const DiagnosticCode& code : report.codes) logInfo(kTag, "  %s", formatDiagnosticCode(code).c_str());
}

void HubDiagnostics::logRawFrame(std::span<const std::uint8_t> frame) {
  if (rawFramesLogged_ >= kRawFrameLogLimit) return;
  rawFramesLogged_++;
  logInfo(kTag, "raw frame via %s: %s", kTransport, spacedHex(frame).c_str());
}

void HubDiagnostics::logSideChannel(std::span<const std::uint8_t> frame) {
  const std::string hex = spacedHex(frame);
  if (sideChannelPayloads_.size() >= kSideChannelPayloadLimit) {
    if (!warnedSideChannelLimit_) {
      warnedSideChannelLimit_ = true;
      logWarning(kTag,
                 "type-31 payload has taken %zu distinct values this run — it is not the constant it looked like; no "
                 "longer logging it.",
                 kSideChannelPayloadLimit);
    }
    return;
  }
  if (!sideChannelPayloads_.insert(hex).second) return;
  logInfo(kTag, "type-31 message via %s: %s", kTransport, hex.c_str());
}

}  // namespace emcool::obd
