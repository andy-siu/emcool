#include "emcool/obd/freeze_frame.hpp"

#include <cmath>
#include <cstdio>

#include "emcool/obd/dtc_table.hpp"
#include "emcool/obd/obd_dtc.hpp"
#include "emcool/text/js.hpp"

namespace emcool::obd {

namespace {

constexpr int kServiceReadDtcInformation = 0x17;
constexpr int kPositiveOffset = 0x40;
constexpr int kNegativeResponse = 0x7f;
constexpr int kHeaderBytes = 5;
constexpr int kTrailingBytes = 1;
constexpr int kCandidateHeaderBytes[] = {kHeaderBytes, 4};

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

bool isSigned(InfokeyDatatype datatype) {
  return datatype == InfokeyDatatype::kInt8 || datatype == InfokeyDatatype::kInt16;
}

FreezeFrameValue readField(const InfokeyField& field, std::span<const std::uint8_t> bytes) {
  double unsignedValue = 0;
  for (const std::uint8_t byte : bytes) unsignedValue = unsignedValue * 256 + byte;
  const int bits = static_cast<int>(bytes.size()) * 8;
  const double raw = isSigned(field.datatype) && unsignedValue >= std::ldexp(1.0, bits - 1)
                         ? unsignedValue - std::ldexp(1.0, bits)
                         : unsignedValue;
  FreezeFrameValue value{field.id, field.name, field.unit, field.datatype, raw, std::nullopt, field.equation,
                         std::nullopt, std::nullopt};
  if (field.refusedScaling != nullptr) {
    value.scalingNote = field.refusedScaling;
    value.scalingRefusal = "impossible-result";
    return value;
  }
  switch (field.scaling) {
    case InfokeyEquation::kIdentity:
      value.value = raw;
      break;
    case InfokeyEquation::kTimes0_1:
      value.value = raw * 0.1;
      break;
    case InfokeyEquation::kTimes0_05625:
      value.value = raw * 0.05625;
      break;
    case InfokeyEquation::kHalfMinus40:
      value.value = raw / 2 - 40;
      break;
    case InfokeyEquation::kMalformed:
      value.scalingNote = "Energica's own equation is malformed";
      value.scalingRefusal = "malformed-equation";
      break;
  }
  return value;
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

std::string hexWithSpaces(std::span<const std::uint8_t> bytes) {
  std::string text;
  char pair[4];
  for (const std::uint8_t byte : bytes) {
    if (!text.empty()) text += ' ';
    std::snprintf(pair, sizeof pair, "%02X", byte);
    text += pair;
  }
  return text;
}

const InfokeyField* lookupInfokey(int id) {
  for (const InfokeyField& field : infokeyTable()) {
    if (field.id == id) return &field;
  }
  return nullptr;
}

const FaultInfokeys* infokeysFor(int component, int symptom) {
  for (const FaultInfokeys& entry : faultInfokeysTable()) {
    if (entry.component == component && entry.symptom == symptom) return &entry;
  }
  return nullptr;
}

int infokeyWidth(InfokeyDatatype datatype) {
  switch (datatype) {
    case InfokeyDatatype::kUint8:
    case InfokeyDatatype::kInt8:
      return 1;
    case InfokeyDatatype::kUint16:
    case InfokeyDatatype::kInt16:
      return 2;
    case InfokeyDatatype::kUint32:
      return 4;
  }
  return 1;
}

const char* infokeyDatatypeName(InfokeyDatatype datatype) {
  switch (datatype) {
    case InfokeyDatatype::kUint8:
      return "uint8_t";
    case InfokeyDatatype::kInt8:
      return "int8_t";
    case InfokeyDatatype::kUint16:
      return "uint16_t";
    case InfokeyDatatype::kInt16:
      return "int16_t";
    case InfokeyDatatype::kUint32:
      return "uint32_t";
  }
  return "uint8_t";
}

int freezeFrameFieldBytes(const FaultInfokeys& shortlist) {
  int total = 0;
  for (const std::uint8_t id : shortlist.infokeys) {
    if (const InfokeyField* field = lookupInfokey(id)) total += infokeyWidth(field->datatype);
  }
  return total;
}

bool isNoStoredRecordReply(std::span<const std::uint8_t> payload) {
  return payload.size() == 2 && payload[0] == kServiceReadDtcInformation + kPositiveOffset && payload[1] == 0;
}

FreezeFrameStatusFlags decodeFreezeFrameStatus(int status) {
  const int activity = (status & 0x06) >> 1;
  return {(status & 0x08) != 0, (status & 0x01) != 0, activity, activity == 2};
}

const char* freezeFrameKindName(FreezeFrameResponse::Kind kind) {
  switch (kind) {
    case FreezeFrameResponse::Kind::kFrame:
      return "frame";
    case FreezeFrameResponse::Kind::kRefused:
      return "refused";
    case FreezeFrameResponse::Kind::kComponentMismatch:
      return "component-mismatch";
    case FreezeFrameResponse::Kind::kUnrecognised:
      return "unrecognised";
  }
  return "unrecognised";
}

FreezeFrameResponse unrecognisedResponse(std::string reason, std::string rawHex) {
  FreezeFrameResponse response;
  response.reason = std::move(reason);
  response.rawHex = std::move(rawHex);
  return response;
}

FreezeFrameResponse decodeFreezeFrameResponse(std::span<const std::uint8_t> payload, int requestedComponent) {
  const std::string rawHex = hexWithSpaces(payload);
  if (payload.empty()) return unrecognisedResponse("empty payload", rawHex);
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) return unrecognisedResponse("negative response without a service and a code", rawHex);
    if (payload[1] != kServiceReadDtcInformation) {
      return unrecognisedResponse("refusal names service " + hex(payload[1]) + ", not 0x17", rawHex);
    }
    FreezeFrameResponse refused;
    refused.kind = FreezeFrameResponse::Kind::kRefused;
    refused.service = payload[1];
    refused.negativeResponseCode = payload[2];
    refused.description = describeNegativeResponseCode(payload[2]);
    refused.rawHex = rawHex;
    return refused;
  }
  if (payload[0] != kServiceReadDtcInformation + kPositiveOffset) {
    return unrecognisedResponse("reply names service " + hex(payload[0]) + ", not 0x57", rawHex);
  }
  if (payload.size() < kHeaderBytes) {
    return unrecognisedResponse("positive reply of " + std::to_string(payload.size()) + " bytes is shorter than the " +
                                    std::to_string(kHeaderBytes) + "-byte header",
                                rawHex);
  }
  const int recordCount = payload[1];
  const int component = (payload[2] << 8) | payload[3];
  const int status = payload[4];
  if (component != requestedComponent) {
    FreezeFrameResponse mismatch;
    mismatch.kind = FreezeFrameResponse::Kind::kComponentMismatch;
    mismatch.requested = requestedComponent;
    mismatch.received = component;
    mismatch.rawHex = rawHex;
    return mismatch;
  }
  FreezeFrameResponse response;
  response.kind = FreezeFrameResponse::Kind::kFrame;
  FreezeFrame& frame = response.frame;
  frame.component = component;
  frame.symptom = (status & 0xf0) >> 4;
  frame.status = status;
  frame.flags = decodeFreezeFrameStatus(status);
  frame.recordCount = recordCount;
  if (const DtcTableEntry* entry = lookupByComponentSymptom(component, frame.symptom)) {
    frame.obdCode = std::string(entry->obdCode);
    frame.description = std::string(entry->description);
  }
  frame.rawHex = rawHex;
  const FaultInfokeys* shortlist = infokeysFor(component, frame.symptom);
  frame.shortlistKnown = shortlist != nullptr;
  const std::span<const std::uint8_t> body = payload.subspan(kHeaderBytes);
  if (shortlist == nullptr) {
    frame.trailingHex = hexWithSpaces(body);
    return response;
  }
  const int fieldBytes = freezeFrameFieldBytes(*shortlist);
  for (const int header : kCandidateHeaderBytes) {
    if (static_cast<int>(payload.size()) - header == fieldBytes) frame.headerBytesThatFit.push_back(header);
  }
  std::size_t offset = 0;
  for (const std::uint8_t id : shortlist->infokeys) {
    const InfokeyField* field = lookupInfokey(id);
    if (field == nullptr) continue;
    const auto width = static_cast<std::size_t>(infokeyWidth(field->datatype));
    if (offset + width > body.size()) {
      frame.truncated = true;
      break;
    }
    frame.values.push_back(readField(*field, body.subspan(offset, width)));
    offset += width;
  }
  const std::span<const std::uint8_t> trailing = body.subspan(offset);
  frame.trailingHex = hexWithSpaces(trailing);
  if (!frame.truncated && trailing.size() == kTrailingBytes) frame.cyclesSinceStored = trailing[0];
  return response;
}

const FreezeFrameValue* fieldOf(const FreezeFrame& frame, std::string_view name) {
  for (const FreezeFrameValue& value : frame.values) {
    if (value.name == name) return &value;
  }
  return nullptr;
}

std::string formatFreezeFrameValue(const FreezeFrameValue& value) {
  if (!value.value) {
    return value.name + " = " + text::jsNumber(value.raw) + " [raw; " + value.scalingNote.value_or("") + ": " +
           value.equation + "]";
  }
  // Number(value.toFixed(3)) for anything not whole.
  const double shown = std::floor(*value.value) == *value.value
                           ? *value.value
                           : std::strtod(text::jsToFixed(*value.value, 3).c_str(), nullptr);
  const std::string unit = value.unit.empty() ? "" : " " + value.unit;
  if (!value.equation.empty()) {
    return value.name + " = " + text::jsNumber(shown) + unit + " (raw " + text::jsNumber(value.raw) + ")";
  }
  return value.name + " = " + text::jsNumber(shown) + unit;
}

void writeFreezeFrameResponseJson(text::TextBuffer& out, const FreezeFrameResponse& response) {
  out.append("{\"kind\":");
  out.appendJsonString(freezeFrameKindName(response.kind));
  switch (response.kind) {
    case FreezeFrameResponse::Kind::kRefused:
      out.append(",\"service\":");
      out.appendInteger(response.service);
      out.append(",\"negativeResponseCode\":");
      out.appendInteger(response.negativeResponseCode);
      out.append(",\"description\":");
      out.appendJsonString(response.description);
      break;
    case FreezeFrameResponse::Kind::kComponentMismatch:
      out.append(",\"requested\":");
      out.appendInteger(response.requested);
      out.append(",\"received\":");
      out.appendInteger(response.received);
      break;
    case FreezeFrameResponse::Kind::kUnrecognised:
      out.append(",\"reason\":");
      out.appendJsonString(response.reason);
      break;
    case FreezeFrameResponse::Kind::kFrame: {
      const FreezeFrame& frame = response.frame;
      out.append(",\"frame\":{\"component\":");
      out.appendInteger(frame.component);
      out.append(",\"symptom\":");
      out.appendInteger(frame.symptom);
      out.append(",\"status\":");
      out.appendInteger(frame.status);
      out.append(frame.flags.lampOn ? ",\"flags\":{\"lampOn\":true" : ",\"flags\":{\"lampOn\":false");
      out.append(frame.flags.stored ? ",\"stored\":true,\"activity\":" : ",\"stored\":false,\"activity\":");
      out.appendInteger(frame.flags.activity);
      out.append(frame.flags.hasFreezeFrame ? ",\"hasFreezeFrame\":true},\"recordCount\":"
                                            : ",\"hasFreezeFrame\":false},\"recordCount\":");
      out.appendInteger(frame.recordCount);
      out.append(",\"obdCode\":");
      writeOptionalString(out, frame.obdCode);
      out.append(",\"description\":");
      writeOptionalString(out, frame.description);
      out.append(",\"values\":[");
      for (std::size_t at = 0; at < frame.values.size(); at++) {
        const FreezeFrameValue& value = frame.values[at];
        if (at > 0) out.append(",");
        out.append("{\"infokey\":");
        out.appendInteger(value.infokey);
        out.append(",\"name\":");
        out.appendJsonString(value.name);
        out.append(",\"unit\":");
        out.appendJsonString(value.unit);
        out.append(",\"datatype\":");
        out.appendJsonString(infokeyDatatypeName(value.datatype));
        out.append(",\"raw\":");
        out.appendNumber(value.raw);
        out.append(",\"value\":");
        if (value.value) {
          out.appendNumber(*value.value);
        } else {
          out.append("null");
        }
        out.append(",\"equation\":");
        out.appendJsonString(value.equation);
        out.append(",\"scalingNote\":");
        writeOptionalString(out, value.scalingNote);
        out.append(",\"scalingRefusal\":");
        writeOptionalString(out, value.scalingRefusal);
        out.append("}");
      }
      out.append(frame.shortlistKnown ? "],\"shortlistKnown\":true" : "],\"shortlistKnown\":false");
      out.append(frame.truncated ? ",\"truncated\":true,\"trailingHex\":" : ",\"truncated\":false,\"trailingHex\":");
      out.appendJsonString(frame.trailingHex);
      out.append(",\"cyclesSinceStored\":");
      if (frame.cyclesSinceStored) {
        out.appendInteger(*frame.cyclesSinceStored);
      } else {
        out.append("null");
      }
      out.append(",\"headerBytesThatFit\":[");
      for (std::size_t at = 0; at < frame.headerBytesThatFit.size(); at++) {
        if (at > 0) out.append(",");
        out.appendInteger(frame.headerBytesThatFit[at]);
      }
      out.append("],\"rawHex\":");
      out.appendJsonString(frame.rawHex);
      out.append("}}");
      return;
    }
  }
  out.append(",\"rawHex\":");
  out.appendJsonString(response.rawHex);
  out.append("}");
}

}  // namespace emcool::obd
