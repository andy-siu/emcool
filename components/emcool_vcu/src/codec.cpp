#include "emcool/vcu/codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "emcool/obd/obd_dtc.hpp"

namespace emcool::vcu {

namespace {

constexpr std::uint8_t kServiceStartSession = 0x10;
constexpr std::uint8_t kServiceTesterPresent = 0x3e;
constexpr std::uint8_t kServiceReadByCommonIdentifier = 0x22;
constexpr std::uint8_t kStandardSession = 0x81;
constexpr std::uint8_t kNegativeResponse = 0x7f;
constexpr int kPositiveOffset = 0x40;
constexpr int kMaxSingleFramePayload = 6;
constexpr int kFirstFramePayloadBytes = 5;
constexpr int kConsecutiveFramePayloadBytes = 6;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

bool isReadOnlyService(std::uint8_t service) {
  return service == kServiceStartSession || service == kServiceTesterPresent ||
         service == kServiceReadByCommonIdentifier;
}

}  // namespace

std::uint8_t targetAddress(Micro micro) { return micro == Micro::kA8 ? 0xa8 : 0xa9; }
const char* microName(Micro micro) { return micro == Micro::kA8 ? "A8" : "A9"; }

std::optional<Micro> microNamed(std::string_view name) {
  if (name.size() != 2 || std::toupper(static_cast<unsigned char>(name[0])) != 'A') return std::nullopt;
  if (name[1] == '8') return Micro::kA8;
  if (name[1] == '9') return Micro::kA9;
  return std::nullopt;
}

int recordLengthFor(StorageType type) {
  switch (type) {
    case StorageType::kBool:
    case StorageType::kByte:
      return 1;
    case StorageType::kWord:
      return 2;
    case StorageType::kDword:
      return 4;
  }
  return 1;
}

const char* storageTypeName(StorageType type) {
  switch (type) {
    case StorageType::kBool:
      return "BOOL";
    case StorageType::kByte:
      return "BYTE";
    case StorageType::kWord:
      return "WORD";
    case StorageType::kDword:
      return "DWORD";
  }
  return "BYTE";
}

std::string toHex(std::span<const std::uint8_t> bytes) {
  std::string text;
  char pair[4];
  for (const std::uint8_t byte : bytes) {
    if (!text.empty()) text += ' ';
    std::snprintf(pair, sizeof pair, "%02X", byte);
    text += pair;
  }
  return text;
}

std::optional<std::uint16_t> identifierFor(int bank, int index) {
  if (bank < 0 || bank > 0xf || index < 0 || index > 0x0fff) return std::nullopt;
  return static_cast<std::uint16_t>((bank << 12) | index);
}

std::vector<std::uint8_t> buildRequestPayload(const VcuRequest& request) {
  std::vector<std::uint8_t> payload;
  switch (request.kind) {
    case RequestKind::kStartSession:
      payload = {kServiceStartSession, kStandardSession};
      break;
    case RequestKind::kTesterPresent:
      payload = {kServiceTesterPresent};
      break;
    case RequestKind::kReadParameter: {
      const std::optional<std::uint16_t> identifier = identifierFor(request.bank, request.index);
      if (!identifier) return {};
      payload = {kServiceReadByCommonIdentifier, static_cast<std::uint8_t>(*identifier >> 8),
                 static_cast<std::uint8_t>(*identifier & 0xff)};
      break;
    }
  }
  // The allowlist, checked again on what was actually built.
  if (payload.empty() || !isReadOnlyService(payload[0])) return {};
  return payload;
}

std::array<std::uint8_t, 8> buildRequestFrame(Micro micro, const VcuRequest& request) {
  const std::vector<std::uint8_t> payload = buildRequestPayload(request);
  std::array<std::uint8_t, 8> frame = {};
  frame[0] = targetAddress(micro);
  frame[1] = static_cast<std::uint8_t>(payload.size());
  std::copy(payload.begin(), payload.end(), frame.begin() + 2);
  return frame;
}

VcuFrame parseResponseFrame(std::span<const std::uint8_t> frame) {
  VcuFrame result;
  if (frame.size() < 2) {
    result.reason = "frame shorter than an address plus a PCI byte";
    return result;
  }
  if (frame[0] != kTesterAddress) {
    result.reason = "addressed to " + hex(frame[0]) + ", not the tester";
    return result;
  }
  const std::uint8_t protocolControl = frame[1];
  switch (protocolControl >> 4) {
    case 0x0: {
      const std::size_t length = protocolControl & 0x0f;
      if (length == 0) {
        result.reason = "single frame declaring zero payload bytes";
        return result;
      }
      if (frame.size() < 2 + length) {
        result.reason = "single frame claims " + std::to_string(length) + " bytes but carries " +
                        std::to_string(frame.size() - 2);
        return result;
      }
      result.kind = VcuFrame::Kind::kPayload;
      result.payload.assign(frame.begin() + 2, frame.begin() + 2 + static_cast<std::ptrdiff_t>(length));
      return result;
    }
    case 0x1:
      if (frame.size() < 3) {
        result.reason = "first frame without its length byte";
        return result;
      }
      result.kind = VcuFrame::Kind::kMultiFrame;
      result.totalLength = ((protocolControl & 0x0f) << 8) | frame[2];
      return result;
    case 0x2:
      result.reason = "consecutive frame of a transfer we are not assembling";
      return result;
    case 0x3:
      result.reason = "flow-control frame, which is ours to send and never to receive";
      return result;
    default:
      result.reason = "unknown PCI " + hex(protocolControl >> 4);
      return result;
  }
}

bool isSessionOpened(std::span<const std::uint8_t> payload) {
  return !payload.empty() && payload[0] == kServiceStartSession + kPositiveOffset;
}

VcuParameterReply decodeParameterReply(std::span<const std::uint8_t> payload, int expectedIdentifier) {
  VcuParameterReply reply;
  if (payload.empty()) {
    reply.reason = "empty payload";
    return reply;
  }
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    reply.kind = VcuParameterReply::Kind::kRefused;
    reply.service = payload[1];
    reply.negativeResponseCode = payload[2];
    reply.description = obd::describeNegativeResponseCode(payload[2]);
    return reply;
  }
  if (payload[0] != kServiceReadByCommonIdentifier + kPositiveOffset) {
    reply.reason = "reply names service " + hex(payload[0]) + ", not 0x62";
    return reply;
  }
  if (payload.size() < 3) {
    reply.reason = "positive reply without a full identifier echo";
    return reply;
  }
  const int received = (payload[1] << 8) | payload[2];
  if (received != expectedIdentifier) {
    reply.kind = VcuParameterReply::Kind::kIdentifierMismatch;
    reply.expected = expectedIdentifier;
    reply.received = received;
    return reply;
  }
  if (payload.size() == 3) {
    reply.reason = "identifier echoed with no record after it";
    return reply;
  }
  reply.kind = VcuParameterReply::Kind::kRecord;
  reply.record.assign(payload.begin() + 3, payload.end());
  return reply;
}

VcuParameterValue interpretRecord(std::span<const std::uint8_t> record, const std::optional<RecordEncoding>& encoding) {
  // In doubles, as JavaScript does, so a record wider than 53 bits rounds the same way.
  double unsignedValue = 0;
  for (const std::uint8_t byte : record) unsignedValue = unsignedValue * 256 + byte;
  const int bits = static_cast<int>(record.size()) * 8;
  const double signedValue = !record.empty() && unsignedValue >= std::ldexp(1.0, bits - 1)
                                 ? unsignedValue - std::ldexp(1.0, bits)
                                 : unsignedValue;
  VcuParameterValue result{toHex(record), unsignedValue, signedValue, std::nullopt, false};
  if (!encoding) return result;
  result.widthMismatch = static_cast<int>(record.size()) != recordLengthFor(encoding->type);
  if (result.widthMismatch || !encoding->isSigned) return result;
  result.value = *encoding->isSigned ? signedValue : unsignedValue;
  return result;
}

std::string describeWidthMismatch(std::size_t recordLength, const RecordEncoding& encoding) {
  return "record is " + std::to_string(recordLength) + " byte(s); " +
         (encoding.describedBy != nullptr ? encoding.describedBy : "the name table") + " says " +
         storageTypeName(encoding.type) + " — value withheld, raw kept";
}

std::vector<std::array<std::uint8_t, 8>> segmentRequestPayload(Micro micro, std::span<const std::uint8_t> payload) {
  std::vector<std::array<std::uint8_t, 8>> frames;
  if (payload.empty() || payload.size() > 0x0fff) return frames;
  const std::uint8_t address = targetAddress(micro);
  if (payload.size() <= kMaxSingleFramePayload) {
    std::array<std::uint8_t, 8> frame = {};
    frame[0] = address;
    frame[1] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), frame.begin() + 2);
    frames.push_back(frame);
    return frames;
  }
  std::array<std::uint8_t, 8> first = {};
  first[0] = address;
  first[1] = static_cast<std::uint8_t>(0x10 | (payload.size() >> 8));
  first[2] = static_cast<std::uint8_t>(payload.size() & 0xff);
  std::copy(payload.begin(), payload.begin() + kFirstFramePayloadBytes, first.begin() + 3);
  frames.push_back(first);
  std::size_t sent = kFirstFramePayloadBytes;
  int sequenceNumber = 0;
  while (sent < payload.size()) {
    std::array<std::uint8_t, 8> consecutive = {};
    consecutive[0] = address;
    consecutive[1] = static_cast<std::uint8_t>(0x20 | (sequenceNumber & 0x0f));
    const std::size_t take = std::min<std::size_t>(kConsecutiveFramePayloadBytes, payload.size() - sent);
    std::copy(payload.begin() + static_cast<std::ptrdiff_t>(sent),
              payload.begin() + static_cast<std::ptrdiff_t>(sent + take), consecutive.begin() + 2);
    frames.push_back(consecutive);
    sent += take;
    sequenceNumber += 1;
  }
  return frames;
}

std::array<std::uint8_t, 8> buildFlowControlFrame(Micro micro) {
  return {targetAddress(micro), 0x30, 0xff, 0x00, 0, 0, 0, 0};
}

std::optional<VcuFlowControl> parseFlowControlFrame(std::span<const std::uint8_t> frame) {
  if (frame.size() < 4 || frame[0] != kTesterAddress || (frame[1] & 0xf0) != 0x30) return std::nullopt;
  switch (frame[1] & 0x0f) {
    case 0x0: {
      const int encoded = frame[3];
      // 0x00-0x7F are milliseconds; 0xF1-0xF9 are 100-900 µs, rounded up to 1 ms; the rest are
      // reserved, and ISO 15765-2 says to treat them as the longest gap.
      const int separation = encoded <= 0x7f ? encoded : (encoded >= 0xf1 && encoded <= 0xf9) ? 1 : 0x7f;
      return VcuFlowControl{VcuFlowControl::Status::kClearToSend, frame[2], separation, 0};
    }
    case 0x1:
      return VcuFlowControl{VcuFlowControl::Status::kWait, 0, 0, 0};
    case 0x2:
      return VcuFlowControl{VcuFlowControl::Status::kOverflow, 0, 0, 0};
    default:
      return VcuFlowControl{VcuFlowControl::Status::kUnrecognised, 0, 0, frame[1] & 0x0f};
  }
}

VcuMultiFrameReply decodeMultiFrameReply(std::span<const std::uint8_t> payload, int expectedService) {
  VcuMultiFrameReply reply;
  if (payload.empty()) {
    reply.reason = "empty payload";
    return reply;
  }
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    if (payload[1] != expectedService - kPositiveOffset) {
      reply.reason = "refusal names service " + hex(payload[1]) + ", not the " + hex(expectedService - kPositiveOffset) +
                     " we asked about";
      return reply;
    }
    reply.kind = VcuMultiFrameReply::Kind::kRefused;
    reply.service = payload[1];
    reply.negativeResponseCode = payload[2];
    reply.description = obd::describeNegativeResponseCode(payload[2]);
    return reply;
  }
  if (payload[0] != expectedService) {
    reply.reason = "reply names service " + hex(payload[0]) + ", not " + hex(expectedService);
    return reply;
  }
  reply.kind = VcuMultiFrameReply::Kind::kPositive;
  reply.service = payload[0];
  reply.body.assign(payload.begin() + 1, payload.end());
  return reply;
}

std::vector<std::uint8_t> encodeMultiFrameRequestPayload(const VcuMultiFrameRequest& request) {
  std::vector<std::uint8_t> payload;
  switch (request.kind) {
    case VcuMultiFrameRequest::Kind::kReadFreezeFrame:
      if (request.component < 1 || request.component > 63) return {};
      payload = {0x17, static_cast<std::uint8_t>((request.component >> 8) & 0xff),
                 static_cast<std::uint8_t>(request.component & 0xff)};
      break;
    case VcuMultiFrameRequest::Kind::kListStoredDtcs:
      payload = {0x18, 0x02, 0xff, 0xff};
      break;
  }
  // The allowlist, checked again on what was built.
  if (payload.empty() || (payload[0] != 0x17 && payload[0] != 0x18)) return {};
  return payload;
}

int expectedResponseService(const VcuMultiFrameRequest& request) {
  return (request.kind == VcuMultiFrameRequest::Kind::kReadFreezeFrame ? 0x17 : 0x18) + kPositiveOffset;
}

VcuStoredDtcList decodeStoredDtcList(std::span<const std::uint8_t> body) {
  VcuStoredDtcList list;
  if (body.empty()) {
    list.truncated = true;
    return list;
  }
  list.declaredCount = body[0];
  std::size_t offset = 1;
  while (offset + 3 <= body.size() && static_cast<int>(list.records.size()) < list.declaredCount) {
    list.records.push_back({(body[offset] << 8) | body[offset + 1], body[offset + 2]});
    offset += 3;
  }
  for (const StoredDtcRecord& record : list.records) {
    if (record.code == 0 && record.status == 0) list.paddingRecords++;
  }
  list.trailingHex = toHex(body.subspan(offset));
  list.truncated = static_cast<int>(list.records.size()) < list.declaredCount;
  return list;
}

int maxFramesFor(int maxPayloadBytes) {
  return (maxPayloadBytes - kFirstFramePayloadBytes + kConsecutiveFramePayloadBytes - 1) /
             kConsecutiveFramePayloadBytes +
         1;
}

ExtendedIsoTpReassembler::ExtendedIsoTpReassembler(int maxPayloadBytes)
    : maxPayloadBytes_(std::max(maxPayloadBytes, kMaxSingleFramePayload + 1)), maxFrames_(maxFramesFor(maxPayloadBytes_)) {}

void ExtendedIsoTpReassembler::reset() {
  payload_.clear();
  filled_ = 0;
  expectedSequenceNumber_ = 0;
  frames_ = 0;
  receiving_ = false;
}

ExtendedIsoTpResult ExtendedIsoTpReassembler::malformed(std::string reason) {
  reset();
  return {ExtendedIsoTpResult::Status::kAbandoned, 0, {}, std::move(reason)};
}

ExtendedIsoTpResult ExtendedIsoTpReassembler::push(std::span<const std::uint8_t> frame) {
  using Status = ExtendedIsoTpResult::Status;
  if (frame.size() < 2) return {Status::kIgnored, 0, {}, "frame shorter than an address plus a PCI byte"};
  if (frame[0] != kTesterAddress) return {Status::kIgnored, 0, {}, "addressed to " + hex(frame[0]) + ", not the tester"};
  if (frames_ >= maxFrames_) {
    reset();
    return {Status::kAbandoned, 0, {}, "more than " + std::to_string(maxFrames_) + " frames in one transfer"};
  }
  frames_ += 1;
  switch (frame[1] >> 4) {
    case 0x0: {
      const int length = frame[1] & 0x0f;
      if (length == 0) return malformed("single frame declaring zero payload bytes");
      if (length > kMaxSingleFramePayload) {
        return malformed("single frame claims " + std::to_string(length) + " bytes, over the " +
                         std::to_string(kMaxSingleFramePayload) + "-byte limit");
      }
      if (static_cast<int>(frame.size()) < 2 + length) {
        return malformed("single frame claims " + std::to_string(length) + " bytes but carries " +
                         std::to_string(frame.size() - 2));
      }
      reset();
      return {Status::kComplete, 0, std::vector<std::uint8_t>(frame.begin() + 2, frame.begin() + 2 + length), {}};
    }
    case 0x1: {
      if (frame.size() < 8) return malformed("first frame shorter than 8 bytes");
      const int totalLength = ((frame[1] & 0x0f) << 8) | frame[2];
      if (totalLength <= kMaxSingleFramePayload) {
        return malformed("first frame declares only " + std::to_string(totalLength) + " bytes");
      }
      if (totalLength > maxPayloadBytes_) {
        reset();
        return {Status::kAbandoned, 0, {},
                "first frame declares " + std::to_string(totalLength) + " bytes, over the " +
                    std::to_string(maxPayloadBytes_) + " cap"};
      }
      payload_.assign(static_cast<std::size_t>(totalLength), 0);
      std::copy(frame.begin() + 3, frame.begin() + 8, payload_.begin());
      filled_ = kFirstFramePayloadBytes;
      expectedSequenceNumber_ = 0;
      frames_ = 1;
      receiving_ = true;
      return {Status::kFlowControlRequired, totalLength, {}, {}};
    }
    case 0x2: {
      if (!receiving_) return {Status::kIgnored, 0, {}, "consecutive frame with no first frame"};
      const int sequenceNumber = frame[1] & 0x0f;
      if (sequenceNumber != expectedSequenceNumber_) {
        const std::string reason = "consecutive frame out of sequence (expected " +
                                   std::to_string(expectedSequenceNumber_) + ", got " + std::to_string(sequenceNumber) +
                                   ")";
        reset();
        return {Status::kAbandoned, 0, {}, reason};
      }
      expectedSequenceNumber_ = (expectedSequenceNumber_ + 1) & 0x0f;
      const int remaining = static_cast<int>(payload_.size()) - filled_;
      const int available = std::max(0, static_cast<int>(frame.size()) - 2);
      const int wanted = std::min(kConsecutiveFramePayloadBytes, remaining);
      if (available < wanted) {
        const std::string reason = "consecutive frame carries " + std::to_string(available) + " bytes where " +
                                   std::to_string(wanted) + " were needed";
        reset();
        return {Status::kAbandoned, 0, {}, reason};
      }
      std::copy(frame.begin() + 2, frame.begin() + 2 + wanted, payload_.begin() + filled_);
      filled_ += wanted;
      if (filled_ < static_cast<int>(payload_.size())) return {Status::kIncomplete, 0, {}, {}};
      std::vector<std::uint8_t> payload = std::move(payload_);
      reset();
      return {Status::kComplete, 0, std::move(payload), {}};
    }
    case 0x3:
      return {Status::kIgnored, 0, {}, "flow-control frame, which is ours to send and never to receive"};
    default:
      return {Status::kIgnored, 0, {}, "unknown PCI " + hex(frame[1] >> 4)};
  }
}

}  // namespace emcool::vcu
