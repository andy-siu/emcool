#pragma once

// Energica's diagnostic-trouble-code table, generated from cool-eva src/diagnostics/dtc-table.ts
// (tools/generate-diagnostic-tables.mts). Keyed by (component, symptom): the OBD column is the
// translation for a generic scan tool and is NOT unique (U0182 is under both 39 and 40), so
// lookupByObdCode() is the lossy way in and returns the first listed.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace emcool::obd {

// The "MIL" column. kUnknown means no source states it, not "no lamp".
enum class Mil : std::uint8_t { kOff, kOn, kUnknown };

struct DtcTableEntry {
  std::uint8_t component;
  std::uint8_t symptom;
  std::string_view obdCode;
  std::string_view name;
  std::string_view description;
  Mil illuminatesMil;
};

std::span<const DtcTableEntry> dtcTable();
const DtcTableEntry* lookupByComponentSymptom(int component, int symptom);
// Case-insensitive, as cool-eva's.
const DtcTableEntry* lookupByObdCode(std::string_view obdCode);

// A 16-bit binary DTC as a scan tool prints it: 0x1046 is "P1046", 0xC111 is "U0111".
struct ObdCodeText {
  std::array<char, 6> characters;
  std::string_view view() const { return {characters.data(), 5}; }
};
ObdCodeText formatObdDtc(std::uint16_t value);

}  // namespace emcool::obd
