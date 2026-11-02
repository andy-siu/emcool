#pragma once

// The OBD-II trouble-code services, ported from cool-eva src/diagnostics/obd-dtc.ts: mode 03
// (stored), 07 (pending) and 0A (permanent). Pure: a reassembled reply in, codes out.
//
// This is a DIFFERENT list from the hub's active faults (the dtc_CCCC_S signals), and a
// different encoding: mode 03 sends the 16-bit binary DTC a scan tool prints, so Energica's
// table is reached through its OBD column. Modes 07 and 0A have never answered on cool-eva's
// bike, and silence is not "nothing pending" — the transport keeps the two apart.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace emcool::obd {

inline constexpr std::uint8_t kModeStoredDtcs = 0x03;
inline constexpr std::uint8_t kModePendingDtcs = 0x07;
inline constexpr std::uint8_t kModePermanentDtcs = 0x0a;

enum class DtcList : std::uint8_t { kStored, kPending, kPermanent };
inline constexpr DtcList kDtcLists[] = {DtcList::kStored, DtcList::kPending, DtcList::kPermanent};
// "stored", "pending" or "permanent", as cool-eva's DtcListKind.
const char* listName(DtcList list);
std::optional<DtcList> listForMode(int mode);

struct ObdDtcResponse {
  enum class Kind : std::uint8_t { kCodes, kNegative, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  // kCodes: the count byte the bike sent, the codes that came (00 00 padding dropped), and
  // whether fewer came than the count promised.
  DtcList list = DtcList::kStored;
  int declaredCount = 0;
  std::vector<std::uint16_t> codes;
  bool truncated = false;
  // kNegative
  int requestedService = 0;
  int negativeResponseCode = 0;
  // kUnrecognised
  std::string reason;
};

// `payload` begins with the response service id: 43/47/4A for a positive reply, 7F for a
// refusal. The service is checked against `requestedMode`, because replies to the poller's
// mode-01 questions share the 0x7E0-0x7EF range.
ObdDtcResponse decodeObdDtcResponse(std::span<const std::uint8_t> payload, int requestedMode);

// "0x11 (serviceNotSupported)", or just the number for a code without a name here.
std::string describeNegativeResponseCode(int negativeResponseCode);

}  // namespace emcool::obd
