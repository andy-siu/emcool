#include "emcool/obd/lifetime.hpp"

#include <cctype>

#include "emcool/text/js.hpp"

namespace emcool::obd {

namespace {

using text::jsNumber;
using text::jsRound;

constexpr double kFullPackAh = 64;
constexpr const char* kRecentAverageNote =
    "a recent average, not a lifetime one — it tracked the season between the two reads";
constexpr const char* kSocContextNote =
    "a full pack always looks tighter than an empty one, so read this against the charge above";

struct Band {
  const char* key;
  double low;
  double high;
};
constexpr Band kBands[] = {
    {"cell_avg_mv", 1500, 4500},  {"cell_min_mv", 1500, 4500},   {"cell_max_mv", 1500, 4500},
    {"cell_min_id", 0, 200},      {"cell_max_id", 0, 200},       {"state_of_health", 0, 100},
    {"state_of_charge", 0, 100},  {"odometer_km", 0, 999999},    {"cell_spread_mv", 0, 2000},
    {"average_battery_temp_c", -40, 80},
};

const Band* bandFor(std::string_view key) {
  for (const Band& band : kBands) {
    if (key == band.key) return &band;
  }
  return nullptr;
}

bool withinBand(std::string_view key, std::optional<double> value) {
  const Band* band = bandFor(key);
  return band == nullptr ? true : value && *value >= band->low && *value <= band->high;
}

struct RowInfo {
  const char* key;
  const char* label;
  int component;
};
constexpr RowInfo kRows[] = {
    {"charges", "charges", 52},
    {"exchanged_ah", "charge moved", 52},
    {"average_battery_temp_c", "average pack temperature", 52},
    {"average_depth_of_discharge", "average depth of discharge", 52},
    {"odometer_km", "odometer", 51},
    {"state_of_health", "state of health", 51},
    {"cell_spread_mv", "cell spread", 51},
};

const RowInfo* rowInfo(std::string_view key) {
  for (const RowInfo& info : kRows) {
    if (key == info.key) return &info;
  }
  return nullptr;
}

LifetimeRow row(const std::string& key) {
  LifetimeRow result;
  result.key = key;
  const RowInfo* info = rowInfo(key);
  result.label = info != nullptr ? info->label : key;
  result.status = "ok";
  return result;
}

LifetimeRow missingRow(const std::string& key, const std::optional<std::string>& outcome = std::nullopt) {
  LifetimeRow result = row(key);
  result.status = "missing";
  const RowInfo* info = rowInfo(key);
  result.note = info == nullptr ? "not in the reply"
                                : "component " + std::to_string(info->component) + " did not answer with a frame" +
                                      (outcome ? " — " + *outcome : "");
  return result;
}

LifetimeRow numberRow(const std::string& key, const std::string& label, const FreezeFrameValue* field,
                      std::optional<std::string> note = std::nullopt) {
  if (field == nullptr) return missingRow(key);
  LifetimeRow result = row(key);
  result.label = label;
  result.unit = field->unit;
  result.raw = field->raw;
  if (!field->value) {
    result.status = "unscaled";
    result.note = field->scalingNote;
    return result;
  }
  const Band* band = bandFor(key);
  if (band != nullptr && !withinBand(key, field->value)) {
    result.status = "rejected";
    result.note = "outside " + jsNumber(band->low) + "…" + jsNumber(band->high) + " " + field->unit +
                  " — a dead sensor or a sentinel, not a reading";
    return result;
  }
  result.value = field->value;
  result.note = std::move(note);
  return result;
}

std::string describeCandidate(double ampHours, const char* label, std::optional<double> odometerKm) {
  const double packs = ampHours / kFullPackAh;
  const std::optional<double> perPack =
      !odometerKm || packs <= 0 ? std::nullopt : std::optional<double>(*odometerKm / packs);
  const std::string range = perPack ? ", ≈" + jsNumber(jsRound(*perPack)) + " km each" : "";
  return "≈" + jsNumber(jsRound(ampHours)) + " Ah at " + label + " — ≈" + jsNumber(jsRound(packs)) + " full packs" + range;
}

LifetimeRow exchangedRow(const FreezeFrameValue* field, std::optional<double> odometerKm) {
  if (field == nullptr) return missingRow("exchanged_ah");
  if (field->value) return numberRow("exchanged_ah", "charge moved", field);
  LifetimeRow result = row("exchanged_ah");
  result.status = "unscaled";
  result.raw = field->raw;
  result.detail = {describeCandidate(field->raw * 0.01, "×0.01", odometerKm),
                   describeCandidate(field->raw / 64, "÷64", odometerKm)};
  result.note = "Energica's own would be " + describeCandidate(field->raw * 0.1, "×0.1", odometerKm) +
                ", which this pack's own logged current refutes by 7.4×. Which of the two above is right is unsettled; "
                "two reads bracketing one charge session would settle it.";
  return result;
}

std::string gated(const FreezeFrameValue* field, std::string_view key) {
  if (field == nullptr) return "–";
  return withinBand(key, field->value) ? jsNumber(field->raw) : "⚠ " + jsNumber(field->raw);
}

LifetimeRow depthOfDischargeRow(const FreezeFrameValue* field) {
  if (field == nullptr) return missingRow("average_depth_of_discharge");
  LifetimeRow result = row("average_depth_of_discharge");
  result.status = "unscaled";
  result.raw = field->raw;
  // JavaScript's & on the raw number: its 32-bit integer.
  const auto bits = static_cast<std::int32_t>(static_cast<std::int64_t>(field->raw));
  result.note = std::to_string(bits & 0xff) +
                " % if Energica's malformed equation means x & 255 — their own tool agreed once, unconfirmed";
  return result;
}

LifetimeRow chargesRow(const FreezeFrameValue* total, const FreezeFrameValue* alternating,
                       const FreezeFrameValue* direct) {
  if (total == nullptr || alternating == nullptr || direct == nullptr) return missingRow("charges");
  const double residue = total->raw - alternating->raw - direct->raw;
  const bool impossible = residue < 0;
  LifetimeRow result = row("charges");
  result.status = impossible ? "rejected" : "ok";
  if (!impossible) result.value = total->raw;
  result.raw = total->raw;
  result.detail = {jsNumber(alternating->raw) + " AC", jsNumber(direct->raw) + " DC", jsNumber(residue) + " neither"};
  result.note = impossible
                    ? "the subtotals exceed the total, which no reading of these counters allows"
                    : "the AC count most likely counts charger cycles rather than plug-ins — 68 of them in the month "
                      "between the two reads, on a bike left plugged in at home. The last number is counted in the total "
                      "but in neither subtotal: aborted or pre-counter sessions, unidentified.";
  return result;
}

LifetimeRow cellSpreadRow(const FreezeFrame& frame) {
  const FreezeFrameValue* minimum = fieldOf(frame, "B_MIN_CELL");
  const FreezeFrameValue* maximum = fieldOf(frame, "B_MAX_CELL");
  if (minimum == nullptr || maximum == nullptr) return missingRow("cell_spread_mv");
  LifetimeRow result = row("cell_spread_mv");
  result.detail = {
      "average " + gated(fieldOf(frame, "B_AVG_CELL"), "cell_avg_mv") + " mV",
      "weakest " + gated(minimum, "cell_min_mv") + " mV (#" + gated(fieldOf(frame, "B_MIN_CELL_ID"), "cell_min_id") + ")",
      "strongest " + gated(maximum, "cell_max_mv") + " mV (#" + gated(fieldOf(frame, "B_MAX_CELL_ID"), "cell_max_id") +
          ")",
      "at " + gated(fieldOf(frame, "B_SOC"), "state_of_charge") + " % charge",
  };
  const bool bounded = withinBand("cell_min_mv", minimum->value) && withinBand("cell_max_mv", maximum->value);
  const double spread = maximum->raw - minimum->raw;
  const bool rejected = !bounded || spread < 0 || !withinBand("cell_spread_mv", spread);
  result.status = rejected ? "rejected" : "ok";
  if (!rejected) {
    result.value = spread;
    result.raw = spread;
  }
  result.unit = "mV";
  result.note = rejected ? "computed from a cell voltage outside 1500…4500 mV — a dead sensor or a sentinel, not a reading"
                         : kSocContextNote;
  return result;
}

const FreezeFrame* frameFor(std::span<const ComponentResponse> responses, int component) {
  for (const ComponentResponse& entry : responses) {
    if (entry.component == component) {
      return entry.response.kind == FreezeFrameResponse::Kind::kFrame ? &entry.response.frame : nullptr;
    }
  }
  return nullptr;
}

std::optional<std::string> outcomeOf(std::span<const ComponentResponse> responses, int component) {
  for (const ComponentResponse& entry : responses) {
    if (entry.component != component) continue;
    if (entry.response.kind == FreezeFrameResponse::Kind::kFrame) return std::nullopt;
    // The first six bytes of the raw text, as split(" ").slice(0, 6).join(" ").
    std::string bytes;
    int taken = 0;
    std::size_t at = 0;
    const std::string& raw = entry.response.rawHex;
    while (taken < 6 && at <= raw.size()) {
      const std::size_t space = raw.find(' ', at);
      const std::string part = raw.substr(at, space == std::string::npos ? std::string::npos : space - at);
      if (taken > 0) bytes += " ";
      bytes += part;
      taken++;
      if (space == std::string::npos) break;
      at = space + 1;
    }
    std::string text = freezeFrameKindName(entry.response.kind);
    if (!bytes.empty()) text += ", " + bytes + "…";
    return text;
  }
  return std::nullopt;
}

bool isTwoHexDigits(const std::string& text) {
  return text.size() == 2 && std::isxdigit(static_cast<unsigned char>(text[0])) &&
         std::isxdigit(static_cast<unsigned char>(text[1])) && !std::islower(static_cast<unsigned char>(text[0])) &&
         !std::islower(static_cast<unsigned char>(text[1]));
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

void writeOptionalNumber(text::TextBuffer& out, const std::optional<double>& value) {
  if (value) {
    out.appendNumber(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

LifetimeStatistics summariseLifetimeStatistics(double readAt, std::span<const ComponentResponse> responses) {
  const FreezeFrame* packState = frameFor(responses, kLifetimePackState);
  const FreezeFrame* counters = frameFor(responses, kLifetimeCounters);
  LifetimeStatistics statistics;
  statistics.readAt = readAt;
  statistics.complete = packState != nullptr && counters != nullptr;
  const std::optional<double> odometer =
      packState != nullptr && fieldOf(*packState, "V_ODOMETER") != nullptr ? fieldOf(*packState, "V_ODOMETER")->value
                                                                            : std::nullopt;
  if (counters == nullptr) {
    const std::optional<std::string> outcome = outcomeOf(responses, kLifetimeCounters);
    for (const char* key : {"charges", "exchanged_ah", "average_battery_temp_c", "average_depth_of_discharge"}) {
      statistics.rows.push_back(missingRow(key, outcome));
    }
  } else {
    statistics.rows.push_back(chargesRow(fieldOf(*counters, "CompletedCharges"), fieldOf(*counters, "CompletedACCharges"),
                                         fieldOf(*counters, "CompletedDCCharges")));
    statistics.rows.push_back(exchangedRow(fieldOf(*counters, "TotalExchangedAh"), odometer));
    statistics.rows.push_back(numberRow("average_battery_temp_c", "average pack temperature",
                                        fieldOf(*counters, "AvgBattTemp"), std::string(kRecentAverageNote)));
    statistics.rows.push_back(depthOfDischargeRow(fieldOf(*counters, "AvgDOD")));
  }
  if (packState == nullptr) {
    for (const char* key : {"odometer_km", "state_of_health", "cell_spread_mv"}) statistics.rows.push_back(missingRow(key));
  } else {
    statistics.rows.push_back(numberRow("odometer_km", "odometer", fieldOf(*packState, "V_ODOMETER")));
    statistics.rows.push_back(numberRow("state_of_health", "state of health", fieldOf(*packState, "B_SOH")));
    statistics.rows.push_back(cellSpreadRow(*packState));
  }
  for (const ComponentResponse& entry : responses) {
    LifetimeComponentOutcome outcome{entry.component, entry.response.kind, std::nullopt, std::nullopt, entry.response.rawHex};
    if (entry.response.kind == FreezeFrameResponse::Kind::kFrame) {
      const FreezeFrame& frame = entry.response.frame;
      outcome.obdCode = frame.obdCode;
      outcome.rawHex = frame.rawHex;
      if (isTwoHexDigits(frame.trailingHex)) outcome.cyclesSinceStored = std::stoi(frame.trailingHex, nullptr, 16);
    }
    statistics.components.push_back(outcome);
  }
  return statistics;
}

void writeLifetimeStatisticsJson(text::TextBuffer& out, const LifetimeStatistics& statistics) {
  out.append("{\"readAt\":");
  out.appendNumber(statistics.readAt);
  out.append(statistics.complete ? ",\"complete\":true,\"rows\":[" : ",\"complete\":false,\"rows\":[");
  for (std::size_t at = 0; at < statistics.rows.size(); at++) {
    const LifetimeRow& row = statistics.rows[at];
    if (at > 0) out.append(",");
    out.append("{\"key\":");
    out.appendJsonString(row.key);
    out.append(",\"label\":");
    out.appendJsonString(row.label);
    out.append(",\"status\":");
    out.appendJsonString(row.status);
    out.append(",\"value\":");
    writeOptionalNumber(out, row.value);
    out.append(",\"unit\":");
    out.appendJsonString(row.unit);
    out.append(",\"raw\":");
    writeOptionalNumber(out, row.raw);
    out.append(",\"detail\":[");
    for (std::size_t item = 0; item < row.detail.size(); item++) {
      if (item > 0) out.append(",");
      out.appendJsonString(row.detail[item]);
    }
    out.append("],\"note\":");
    writeOptionalString(out, row.note);
    out.append("}");
  }
  out.append("],\"components\":[");
  for (std::size_t at = 0; at < statistics.components.size(); at++) {
    const LifetimeComponentOutcome& outcome = statistics.components[at];
    if (at > 0) out.append(",");
    out.append("{\"component\":");
    out.appendInteger(outcome.component);
    out.append(",\"kind\":");
    out.appendJsonString(freezeFrameKindName(outcome.kind));
    out.append(",\"obdCode\":");
    writeOptionalString(out, outcome.obdCode);
    out.append(",\"cyclesSinceStored\":");
    if (outcome.cyclesSinceStored) {
      out.appendInteger(*outcome.cyclesSinceStored);
    } else {
      out.append("null");
    }
    out.append(",\"rawHex\":");
    out.appendJsonString(outcome.rawHex);
    out.append("}");
  }
  out.append("]}");
}

}  // namespace emcool::obd
