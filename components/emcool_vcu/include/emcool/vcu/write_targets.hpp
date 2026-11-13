#pragma once

// THE ALLOWLIST: the only VCU calibration parameters this repo will write, the only values it will
// write into them, and the pure function that turns a request into bytes or into a refusal. Data and
// arithmetic only.
//
// An identifier not on the list is UNEXPRESSIBLE: the HTTP layer takes a NAME, never a number,
// and write_codec.cpp re-checks the plan against this module immediately before the bytes are
// built. Every bound is POLICY; every name is a claim about which of Energica's parameter tables
// the bike runs.
//
// The active parameter table is threaded through as a `ParameterNames&`, as the read path does,
// so there is no global write-path state.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/tables.hpp"

namespace emcool::vcu {

struct WritableBit {
  std::string key;  // what a caller names it; lower-case, no spaces (appears in URLs and the audit)
  int mask;
  std::string label;
  std::string caveat;  // shown before the confirmation, every time
};

struct WriteControl {
  enum class Kind : std::uint8_t { kNumber, kBits };
  Kind kind = Kind::kNumber;
  // kNumber: raw counts, the units the record carries.
  double min = 0;
  double max = 0;
  // kBits.
  std::vector<WritableBit> bits;
};

// How a target's raw count reads to a human.
enum class UnitKind : std::uint8_t { kAmps, kRawUnknown, kDeciNm, kHexWord, kPlain };

struct WriteTarget {
  std::string name;  // params.ecf's name, the primary key
  int index = 0;     // 1-based params.ecf index; the rest is looked up
  WriteControl control;
  UnitKind unitKind = UnitKind::kPlain;
  std::string purpose;
  std::vector<std::string> warnings;
  // How to check the bike afterwards, beyond the read-back; nullopt where nothing else can.
  std::optional<std::string> verify;
  // target.unit(raw): what the raw count means to a human.
  std::string unit(double raw) const;
};

// A write, fully decided and ready to become bytes. Only planWrite/planBitWrite produce one.
struct ParameterWritePlan {
  std::string name;
  int index = 0;
  Micro micro = Micro::kA9;
  int identifier = 0;                 // 0x1000 | index
  std::vector<std::uint8_t> record;   // exactly the bytes after the identifier
  double value = 0;                   // read per the table's S/U column
  double previousValue = 0;           // what the bike held, as the caller read it
  std::string description;            // "MAX_DC_CHG_CURRENT: 75 A -> 80 A"
};

struct WritePlanOutcome {
  bool ok = false;
  ParameterWritePlan plan;
  std::string reason;  // when !ok
};

// The five researched targets, by name, in order. Constant; no active table needed.
std::vector<WriteTarget> curatedWriteTargets();
// The researched five, then the rest of the bike's own (active) table (minus duplicated names).
std::vector<WriteTarget> writeTargets(const ParameterNames& names);
std::vector<std::string> writeTargetNames(const ParameterNames& names);
// The target with this name, or nullopt. Case-insensitive.
std::optional<WriteTarget> writeTargetNamed(const ParameterNames& names, std::string_view name);

WritePlanOutcome planWrite(const ParameterNames& names, std::string_view name, double requestedValue,
                           double previousValue);
WritePlanOutcome planBitWrite(const ParameterNames& names, std::string_view name, std::string_view bitKey, bool on,
                              double currentValue);

// Why writing this target on a bike running `bikeTable` would be unsafe, or nullopt.
std::optional<std::string> writeTargetProblemIn(const ParameterNames& names, const WriteTarget& target,
                                                const VcuParameterTable& bikeTable);
// Everything wrong with writing the curated allowlist on a bike running `bikeTable`. Empty = none.
std::vector<std::string> allowlistProblemsIn(const ParameterNames& names, const VcuParameterTable& bikeTable);

}  // namespace emcool::vcu
