#include "emcool/obd/stored_codes.hpp"

#include "emcool/log.hpp"
#include "emcool/obd/dtc_table.hpp"

namespace emcool::obd {

namespace {

constexpr const char* kTag = "obd-dtc";

const char* countSignalKey(DtcList list) {
  switch (list) {
    case DtcList::kStored:
      return "dtc_stored_count";
    case DtcList::kPending:
      return "dtc_pending_count";
    case DtcList::kPermanent:
      return "dtc_permanent_count";
  }
  return "dtc_stored_count";
}

TroubleCodeListState toListState(const DtcReadOutcome& result) {
  TroubleCodeListState state;
  switch (result.outcome) {
    case DtcReadOutcome::Kind::kSilent:
      state.state = TroubleCodeListState::State::kNoResponse;
      return state;
    case DtcReadOutcome::Kind::kNotSent:
      state.state = TroubleCodeListState::State::kNotAsked;
      state.reason = result.reason;
      return state;
    case DtcReadOutcome::Kind::kTruncated:
      state.state = TroubleCodeListState::State::kIncomplete;
      state.reason = result.reason;
      return state;
    case DtcReadOutcome::Kind::kAnswered:
      break;
  }
  const ObdDtcResponse& response = result.response;
  if (response.kind == ObdDtcResponse::Kind::kNegative) {
    state.state = TroubleCodeListState::State::kRefused;
    state.negativeResponseCode = response.negativeResponseCode;
    return state;
  }
  if (response.kind == ObdDtcResponse::Kind::kUnrecognised) {
    state.state = TroubleCodeListState::State::kUnrecognised;
    state.reason = response.reason;
    return state;
  }
  state.state = TroubleCodeListState::State::kCodes;
  state.codes = response.codes;
  state.declaredCount = response.declaredCount;
  state.truncated = response.truncated;
  return state;
}

// " [MIL]", " [MIL?]" (no source states it) or nothing.
const char* milSuffix(Mil mil) {
  switch (mil) {
    case Mil::kOn:
      return " [MIL]";
    case Mil::kUnknown:
      return " [MIL?]";
    case Mil::kOff:
      return "";
  }
  return "";
}

void writeNullOr(text::TextBuffer& out, const DtcTableEntry* entry, std::string_view DtcTableEntry::*field) {
  if (entry == nullptr) {
    out.append("null");
  } else {
    out.appendJsonString(entry->*field);
  }
}

// cool-eva TroubleCodeRow: the code with Energica's row folded in, or nulls where the table
// doesn't list it. An empty obdCode is the freeze frame's "none stored".
void writeRow(text::TextBuffer& out, std::uint16_t raw, std::string_view obdCode) {
  const DtcTableEntry* entry = obdCode.empty() ? nullptr : lookupByObdCode(obdCode);
  out.append("{\"raw\":");
  out.appendUnsigned(raw);
  out.append(",\"obdCode\":");
  out.appendJsonString(obdCode);
  out.append(",\"component\":");
  if (entry == nullptr) {
    out.append("null,\"symptom\":null");
  } else {
    out.appendUnsigned(entry->component);
    out.append(",\"symptom\":");
    out.appendUnsigned(entry->symptom);
  }
  out.append(",\"name\":");
  writeNullOr(out, entry, &DtcTableEntry::name);
  out.append(",\"description\":");
  writeNullOr(out, entry, &DtcTableEntry::description);
  out.append(",\"illuminatesMil\":");
  if (entry == nullptr || entry->illuminatesMil == Mil::kUnknown) {
    out.append("null");
  } else {
    out.append(entry->illuminatesMil == Mil::kOn ? "true" : "false");
  }
  out.append("}");
}

void writeListState(text::TextBuffer& out, const TroubleCodeListState& list) {
  switch (list.state) {
    case TroubleCodeListState::State::kNotRead:
      out.append("{\"state\":\"not-read\"}");
      return;
    case TroubleCodeListState::State::kNoResponse:
      out.append("{\"state\":\"no-response\"}");
      return;
    case TroubleCodeListState::State::kNotAsked:
    case TroubleCodeListState::State::kIncomplete:
    case TroubleCodeListState::State::kUnrecognised:
      out.append(list.state == TroubleCodeListState::State::kNotAsked    ? "{\"state\":\"not-asked\""
                 : list.state == TroubleCodeListState::State::kIncomplete ? "{\"state\":\"incomplete\""
                                                                          : "{\"state\":\"unrecognised\"");
      out.append(",\"reason\":");
      out.appendJsonString(list.reason);
      out.append("}");
      return;
    case TroubleCodeListState::State::kRefused:
      out.append("{\"state\":\"refused\",\"negativeResponseCode\":");
      out.appendInteger(list.negativeResponseCode);
      out.append(",\"description\":");
      out.appendJsonString(describeNegativeResponseCode(list.negativeResponseCode));
      out.append("}");
      return;
    case TroubleCodeListState::State::kCodes:
      break;
  }
  out.append("{\"state\":\"codes\",\"codes\":[");
  bool first = true;
  for (const std::uint16_t raw : list.codes) {
    if (!first) out.append(",");
    first = false;
    writeRow(out, raw, formatObdDtc(raw).view());
  }
  out.append("],\"declaredCount\":");
  out.appendInteger(list.declaredCount);
  out.append(list.truncated ? ",\"truncated\":true}" : ",\"truncated\":false}");
}

}  // namespace

void StoredCodes::recordRead(const DtcReadOutcome& result, DtcList list, Now now, SignalRecorder& recorder) {
  const int index = static_cast<int>(list);
  const std::string line = describeReadOutcome(result);
  lists_[index] = toListState(result);
  const TroubleCodeListState& current = lists_[index];
  if (current.state == TroubleCodeListState::State::kCodes) {
    recorder.record(countSignalKey(list), static_cast<double>(current.codes.size()));
  }
  // Only when a question reached the bus: "last read 3 s ago" beside a list we failed to send
  // would be a freshness we don't have.
  if (current.state != TroubleCodeListState::State::kNotAsked) {
    readAtWallClockMs_ = now.wallClockMs;
    readAtMonotonicMs_ = now.monotonicMs;
  }
  // Signed on the codes, not the summary: a service that cleared one code and set another
  // leaves the count alone, and this log is the only durable record of which codes they were.
  std::string signature = line;
  if (current.state == TroubleCodeListState::State::kCodes) {
    signature += " :: ";
    bool first = true;
    for (const std::uint16_t raw : current.codes) {
      if (!first) signature += ",";
      first = false;
      signature += formatObdDtc(raw).view();
    }
  }
  if (lastSignature_[index] == signature) return;
  lastSignature_[index] = std::move(signature);
  logInfo(kTag, "%s", line.c_str());
  if (current.state != TroubleCodeListState::State::kCodes) return;
  for (const std::uint16_t raw : current.codes) {
    const ObdCodeText code = formatObdDtc(raw);
    const DtcTableEntry* entry = lookupByObdCode(code.view());
    const std::string named = entry != nullptr ? std::string(entry->description) : "not in Energica's table";
    logInfo(kTag, "  %s — %s%s", code.characters.data(), named.c_str(),
            milSuffix(entry != nullptr ? entry->illuminatesMil : Mil::kUnknown));
  }
}

void StoredCodes::recordFreezeFrameDtc(std::uint16_t raw) {
  const bool changed = freezeFrame_ != raw;
  freezeFrame_ = raw;
  if (!changed) return;
  if (raw == 0) {
    logInfo(kTag, "freeze-frame DTC = 0000 (no freeze frame stored)");
    return;
  }
  const ObdCodeText code = formatObdDtc(raw);
  const DtcTableEntry* entry = lookupByObdCode(code.view());
  const std::string description = entry != nullptr ? std::string(entry->description) : "not in the table";
  logInfo(kTag, "freeze-frame DTC = %s — %s", code.characters.data(), description.c_str());
}

void StoredCodes::writeJson(text::TextBuffer& out, std::int64_t nowMonotonicMs) const {
  out.append("{\"stored\":");
  writeListState(out, lists_[0]);
  out.append(",\"pending\":");
  writeListState(out, lists_[1]);
  out.append(",\"permanent\":");
  writeListState(out, lists_[2]);
  out.append(",\"freezeFrame\":");
  if (!freezeFrame_) {
    out.append("null");
  } else if (*freezeFrame_ == 0) {
    writeRow(out, 0, "");
  } else {
    writeRow(out, *freezeFrame_, formatObdDtc(*freezeFrame_).view());
  }
  out.append(",\"readAt\":");
  if (readAtWallClockMs_) {
    out.appendInteger(*readAtWallClockMs_);
  } else {
    out.append("null");
  }
  out.append(",\"ageMs\":");
  if (readAtMonotonicMs_) {
    out.appendInteger(nowMonotonicMs - *readAtMonotonicMs_);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace emcool::obd
