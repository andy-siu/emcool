#include <cctype>

#include "emcool/obd/dtc_table.hpp"

namespace emcool::obd {

const DtcTableEntry* lookupByComponentSymptom(int component, int symptom) {
  for (const DtcTableEntry& entry : dtcTable()) {
    if (entry.component == component && entry.symptom == symptom) return &entry;
  }
  return nullptr;
}

const DtcTableEntry* lookupByObdCode(std::string_view obdCode) {
  // The first listed wins: U0182 is under both 39 and 40, and the document lists 39 first.
  for (const DtcTableEntry& entry : dtcTable()) {
    if (entry.obdCode.size() != obdCode.size()) continue;
    bool same = true;
    for (std::size_t index = 0; index < obdCode.size() && same; index++) {
      same = std::toupper(static_cast<unsigned char>(obdCode[index])) == entry.obdCode[index];
    }
    if (same) return &entry;
  }
  return nullptr;
}

ObdCodeText formatObdDtc(std::uint16_t value) {
  constexpr char kLetters[] = "PCBU";
  constexpr char kHex[] = "0123456789ABCDEF";
  ObdCodeText text = {};
  text.characters[0] = kLetters[(value >> 14) & 3];
  text.characters[1] = static_cast<char>('0' + ((value >> 12) & 3));
  text.characters[2] = kHex[(value >> 8) & 0xf];
  text.characters[3] = kHex[(value >> 4) & 0xf];
  text.characters[4] = kHex[value & 0xf];
  text.characters[5] = '\0';
  return text;
}

}  // namespace emcool::obd
