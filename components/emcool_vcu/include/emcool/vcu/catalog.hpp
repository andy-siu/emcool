#pragma once

// Which parameter table names the bike's parameters, ported from cool-eva src/vcu/table-catalog.ts,
// src/vcu/param-table.ts and src/vcu/a8-firmware-rows.ts: the table in use (cool-eva's module-level
// `active`, here an object), switching it when a sweep reads the bike's own TABLE_TYPE, and the
// verdict on that TABLE_TYPE.

#include <optional>
#include <string>
#include <vector>

#include "emcool/vcu/tables.hpp"

namespace emcool::vcu {

// The two indices that hold TABLE_TYPE, one per micro.
inline constexpr int kTableTypeIndices[] = {276, 277};
bool isTableTypeIndex(int index);

// "16407 (0x4017 — family 4, revision 0x017)".
std::string describeTableType(double raw);
// The other tables with byte-identical content (same fingerprint), ascending.
std::vector<int> contentTwinsOf(int tableType);
// "4102, 4104, …": the tables carried.
std::string knownTableTypesText();

const A8FirmwareRow* firmwareRowFor(Micro micro, int bank, int index);
std::string describeFirmwareRow(const A8FirmwareRow& row);

struct TableTypeVerdict {
  int index;
  Micro micro;
  double value;
  bool recognised;
  std::string message;
};

class ParameterNames {
 public:
  // cool-eva's DEFAULT_TABLE_TYPE, until a sweep or the stored snapshot says otherwise.
  ParameterNames();

  const VcuParameterTable& active() const { return *active_; }
  const VcuParameter* atIndex(int index) const { return active_->atIndex(index); }

  struct Selection {
    bool ok;
    int tableType;
    std::string message;
  };
  // selectParameterTable(): switches to a carried table, logging what changed; an uncarried one
  // keeps the current names, with a warning.
  Selection select(int tableType);
  // checkTableType(): nullopt unless `index` is a TABLE_TYPE index the active table has.
  std::optional<TableTypeVerdict> checkTableType(int index, double value) const;

 private:
  const VcuParameterTable* active_;
};

}  // namespace emcool::vcu
