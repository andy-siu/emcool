#pragma once

// A freeze frame — what the VCU recorded when a code set — decoded, ported from cool-eva
// src/diagnostics/freeze-frame.ts. Pure: the KWP `57` reply to `17 <component>` in, its fields out.
//
// The reply is a 5-byte header (record count, component, a status byte whose high nibble is the
// symptom), then the fields Energica's shortlist names for that (component, symptom), in that
// order, then one trailing byte — the cycles since it was stored. Keyed by the VCU's component
// number, NOT the 16-bit DTC mode 03 sends.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/obd/infokeys.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::obd {

inline constexpr int kFreezeFrameMinComponent = 1;
inline constexpr int kFreezeFrameMaxComponent = 63;

// `57 00`: the micro has nothing stored for this component — an answer, not a failure.
bool isNoStoredRecordReply(std::span<const std::uint8_t> payload);

struct FreezeFrameStatusFlags {
  bool lampOn;
  bool stored;
  int activity;
  bool hasFreezeFrame;
};
FreezeFrameStatusFlags decodeFreezeFrameStatus(int status);

struct FreezeFrameValue {
  int infokey;
  std::string name;
  std::string unit;
  InfokeyDatatype datatype;
  double raw;
  std::optional<double> value;  // scaled, unless the scaling is refused
  std::string equation;
  std::optional<std::string> scalingNote;
  std::optional<std::string> scalingRefusal;  // "malformed-equation" | "impossible-result"
};

struct FreezeFrame {
  int component = 0;
  int symptom = 0;
  int status = 0;
  FreezeFrameStatusFlags flags = {};
  int recordCount = 0;
  std::optional<std::string> obdCode;
  std::optional<std::string> description;
  std::vector<FreezeFrameValue> values;
  bool shortlistKnown = false;
  bool truncated = false;
  std::string trailingHex;
  std::optional<int> cyclesSinceStored;
  std::vector<int> headerBytesThatFit;
  std::string rawHex;
};

struct FreezeFrameResponse {
  enum class Kind : std::uint8_t { kFrame, kRefused, kComponentMismatch, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  FreezeFrame frame;
  int service = 0;
  int negativeResponseCode = 0;
  std::string description;
  int requested = 0;
  int received = 0;
  std::string reason;
  std::string rawHex;
};
const char* freezeFrameKindName(FreezeFrameResponse::Kind kind);

FreezeFrameResponse decodeFreezeFrameResponse(std::span<const std::uint8_t> payload, int requestedComponent);
// A stored reply that failed, as cool-eva's stores rebuild one: unrecognised, with a reason.
FreezeFrameResponse unrecognisedResponse(std::string reason, std::string rawHex);

// "NAME = 12.5 V (raw 125)".
std::string formatFreezeFrameValue(const FreezeFrameValue& value);
const FreezeFrameValue* fieldOf(const FreezeFrame& frame, std::string_view name);

void writeFreezeFrameResponseJson(text::TextBuffer& out, const FreezeFrameResponse& response);

// "0A FF": cool-eva's toHex, upper case with spaces.
std::string hexWithSpaces(std::span<const std::uint8_t> bytes);

}  // namespace emcool::obd
