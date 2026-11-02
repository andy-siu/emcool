#include "emcool/obd/obd_dtc.hpp"

#include <cstdio>

namespace emcool::obd {

namespace {

constexpr int kPositiveResponseOffset = 0x40;
constexpr int kNegativeResponseSid = 0x7f;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

}  // namespace

const char* listName(DtcList list) {
  switch (list) {
    case DtcList::kStored:
      return "stored";
    case DtcList::kPending:
      return "pending";
    case DtcList::kPermanent:
      return "permanent";
  }
  return "stored";
}

std::optional<DtcList> listForMode(int mode) {
  if (mode == kModeStoredDtcs) return DtcList::kStored;
  if (mode == kModePendingDtcs) return DtcList::kPending;
  if (mode == kModePermanentDtcs) return DtcList::kPermanent;
  return std::nullopt;
}

ObdDtcResponse decodeObdDtcResponse(std::span<const std::uint8_t> payload, int requestedMode) {
  ObdDtcResponse response;
  if (payload.empty()) {
    response.reason = "empty payload";
    return response;
  }
  const int responseService = payload[0];
  if (responseService == kNegativeResponseSid) {
    // `7F <service> <NRC>`; a truncated refusal is still a refusal.
    response.kind = ObdDtcResponse::Kind::kNegative;
    response.requestedService = payload.size() > 1 ? payload[1] : 0;
    response.negativeResponseCode = payload.size() > 2 ? payload[2] : 0;
    return response;
  }
  const std::optional<DtcList> list = listForMode(requestedMode);
  if (!list) {
    response.reason = "mode " + hex(requestedMode) + " is not a trouble-code service";
    return response;
  }
  if (responseService != requestedMode + kPositiveResponseOffset) {
    response.reason = "service " + hex(responseService) + " answering a mode " + hex(requestedMode) + " request";
    return response;
  }
  // The count byte is a cross-check, never the loop bound: the payload's own length decides
  // how many codes are read, so a wrong count can't walk off the end.
  response.kind = ObdDtcResponse::Kind::kCodes;
  response.list = *list;
  response.declaredCount = payload.size() >= 2 ? payload[1] : 0;
  const std::span<const std::uint8_t> body = payload.size() > 2 ? payload.subspan(2) : payload.subspan(payload.size());
  for (std::size_t offset = 0; offset + 1 < body.size(); offset += 2) {
    const auto raw = static_cast<std::uint16_t>((body[offset] << 8) | body[offset + 1]);
    // `00 00` is an empty slot (padding), not code P0000.
    if (raw == 0) continue;
    response.codes.push_back(raw);
  }
  response.truncated = static_cast<int>(response.codes.size()) < response.declaredCount;
  return response;
}

std::string describeNegativeResponseCode(int negativeResponseCode) {
  const char* name = nullptr;
  switch (negativeResponseCode) {
    case 0x10:
      name = "generalReject";
      break;
    case 0x11:
      name = "serviceNotSupported";
      break;
    case 0x12:
      name = "subFunctionNotSupported";
      break;
    case 0x21:
      name = "busyRepeatRequest";
      break;
    case 0x22:
      name = "conditionsNotCorrect";
      break;
    case 0x31:
      name = "requestOutOfRange";
      break;
    case 0x33:
      name = "securityAccessDenied";
      break;
    case 0x78:
      name = "responsePending";
      break;
    default:
      break;
  }
  char text[48];
  if (name != nullptr) {
    std::snprintf(text, sizeof text, "0x%02x (%s)", negativeResponseCode, name);
  } else {
    std::snprintf(text, sizeof text, "0x%02x", negativeResponseCode);
  }
  return text;
}

}  // namespace emcool::obd
