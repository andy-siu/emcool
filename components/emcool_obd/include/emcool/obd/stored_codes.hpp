#pragma once

// What emcool knows about the bike's three trouble-code lists, ported from cool-eva
// src/diagnostics/stored-codes.ts: the latest answer for each, their counts as signals, the
// freeze-frame code from mode 01 PID 02, and /stored-dtcs' body.
//
// THE DISTINCTION THIS EXISTS TO PROTECT: "the bike says there are no codes" and "the bike
// said nothing" are different claims. A count is recorded only when the bike answered, and a
// list that was asked and got silence stays `no-response` all the way to the screen.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/obd/bus.hpp"
#include "emcool/obd/obd_dtc.hpp"
#include "emcool/obd/trouble_code_reader.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::obd {

inline constexpr const char* kFreezeFrameDtcKey = "freeze_frame_dtc";

struct TroubleCodeListState {
  enum class State : std::uint8_t { kNotRead, kCodes, kNoResponse, kNotAsked, kIncomplete, kRefused, kUnrecognised };
  State state = State::kNotRead;
  // kCodes: the raw 16-bit codes; their names come from the table when written out.
  std::vector<std::uint16_t> codes;
  int declaredCount = 0;
  bool truncated = false;
  // kRefused
  int negativeResponseCode = 0;
  // kNotAsked, kIncomplete and kUnrecognised
  std::string reason;
};

class StoredCodes {
 public:
  // recordTroubleCodeRead(): files one read's outcome, records the count signal if the bike
  // answered, and logs the list when it first appears or changes.
  void recordRead(const DtcReadOutcome& result, DtcList list, Now now, SignalRecorder& recorder);
  // recordFreezeFrameDtc(): mode 01 PID 02's code; 0 is the bike saying none is stored.
  void recordFreezeFrameDtc(std::uint16_t raw);

  const TroubleCodeListState& list(DtcList list) const { return lists_[static_cast<int>(list)]; }

  // JSON.stringify(troubleCodeSnapshot()), as /stored-dtcs serves it.
  void writeJson(text::TextBuffer& out, std::int64_t nowMonotonicMs) const;

 private:
  TroubleCodeListState lists_[3];
  std::optional<std::string> lastSignature_[3];
  std::optional<std::uint16_t> freezeFrame_;
  std::optional<std::int64_t> readAtWallClockMs_;
  std::optional<std::int64_t> readAtMonotonicMs_;
};

}  // namespace emcool::obd
