#pragma once

// Whether the bike has named a parameter table this software can write against. A parameter is
// written BY INDEX, and an index only means a parameter relative to a table, so a write is
// refused unless both micros named a carried table AND the allowlist's names mean there what
// they mean here. Reads are never gated.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/snapshot.hpp"

namespace emcool::vcu {

enum class TableGateState : std::uint8_t {
  kConfirmed,
  kMismatched,
  kSplit,
  kUnwritable,
  kUnusable,
  kUnread,
};
const char* tableGateStateName(TableGateState state);

struct TableGateVerdict {
  TableGateState state = TableGateState::kUnread;
  bool writesAllowed = false;
  bool noReadWillHelp = false;
  std::string reason;
  std::string remedy;
  std::vector<int> outstanding;
  std::optional<int> tableType;
};

// `report` is the last sweep's TABLE_TYPE report, or nullptr when none; `names` is the active table.
TableGateVerdict evaluateTableGate(const TableTypeReport* report, const ParameterNames& names);
void writeTableGateVerdictJson(text::TextBuffer& out, const TableGateVerdict& verdict);

}  // namespace emcool::vcu
