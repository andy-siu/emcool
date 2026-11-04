#pragma once

// The bike's ACTIVE faults, as the Energica Connectivity Hub mirrors them onto CAN 0x410. Passive: it
// only listens.
//
// 0x410 message type 25 carries the list a page at a time, two 20-bit codes a page — (component,
// symptom) as Energica's own table keys them, not the 16-bit DTC mode 03 sends. Each complete list
// sets dtc_CCCC_S to 1 for a code present and 0 for one seen before and gone, plus
// dtc_list_count and dtc_unrecognised_count. Type 31 is a side channel, logged while it varies.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "emcool/obd/bus.hpp"
#include "emcool/obd/dtc_table.hpp"

namespace emcool::obd {

struct DiagnosticCode {
  std::uint32_t raw;
  int component;
  int symptom;
  std::string obdCodeFromRaw;
  int flags;
  const DtcTableEntry* entry;
  enum class MatchedBy : std::uint8_t { kNone, kComponent, kObd } matchedBy;
};
DiagnosticCode describeDiagnosticCode(std::uint32_t raw, int flags);
std::string formatDiagnosticCode(const DiagnosticCode& code);

struct DiagnosticReport {
  std::vector<DiagnosticCode> codes;
  int pages;
};

class DiagnosticListAssembler {
 public:
  std::optional<DiagnosticReport> push(std::span<const std::uint8_t> frame);
  void reset();

 private:
  std::vector<DiagnosticCode> codes_;
  int pages_ = 0;
};

bool isDiagnosticsMessage(std::span<const std::uint8_t> frame);
bool isDiagnosticsInfoMessage(std::span<const std::uint8_t> frame);

// The hub-mirror handler, with a memory of what it has seen and logged.
class HubDiagnostics {
 public:
  void handle(std::span<const std::uint8_t> frame, SignalRecorder& recorder);

 private:
  void recordReport(const DiagnosticReport& report, SignalRecorder& recorder);
  void logRawFrame(std::span<const std::uint8_t> frame);
  void logSideChannel(std::span<const std::uint8_t> frame);

  DiagnosticListAssembler assembler_;
  std::set<std::string> seenSignalKeys_;
  std::vector<std::string> seenOrder_;
  bool warnedSeenKeyLimit_ = false;
  std::optional<std::string> lastSignature_;
  int rawFramesLogged_ = 0;
  std::set<std::string> sideChannelPayloads_;
  bool warnedSideChannelLimit_ = false;
};

}  // namespace emcool::obd
