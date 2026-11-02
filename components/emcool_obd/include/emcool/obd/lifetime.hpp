#pragma once

// The pack's lifetime statistics, ported from cool-eva src/diagnostics/lifetime-stats.ts,
// lifetime-rows.ts and lifetime-bands.ts: two freeze-frame components the VCU always answers —
// 51 (pack state: odometer, SOH, cells) and 52 (counters: charges, charge moved, averages) —
// summarised into rows, with the readings no sensor could give rejected rather than shown.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/obd/freeze_frame.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::obd {

inline constexpr int kLifetimePackState = 51;
inline constexpr int kLifetimeCounters = 52;

struct LifetimeRow {
  std::string key;
  std::string label;
  std::string status;  // ok | unscaled | rejected | missing
  std::optional<double> value;
  std::string unit;
  std::optional<double> raw;
  std::vector<std::string> detail;
  std::optional<std::string> note;
};

struct LifetimeComponentOutcome {
  int component;
  FreezeFrameResponse::Kind kind;
  std::optional<std::string> obdCode;
  std::optional<int> cyclesSinceStored;
  std::string rawHex;
};

struct LifetimeStatistics {
  double readAt = 0;
  bool complete = false;
  std::vector<LifetimeRow> rows;
  std::vector<LifetimeComponentOutcome> components;
};

struct ComponentResponse {
  int component;
  FreezeFrameResponse response;
};
LifetimeStatistics summariseLifetimeStatistics(double readAt, std::span<const ComponentResponse> responses);
void writeLifetimeStatisticsJson(text::TextBuffer& out, const LifetimeStatistics& statistics);

}  // namespace emcool::obd
