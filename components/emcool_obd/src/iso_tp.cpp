#include "emcool/obd/iso_tp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace emcool::obd {

namespace {

constexpr std::uint8_t kSingleFrame = 0x0;
constexpr std::uint8_t kFirstFrame = 0x1;
constexpr std::uint8_t kConsecutiveFrame = 0x2;
constexpr std::uint8_t kFlowControlFrame = 0x3;

static_assert(IsoTpReassembler::kMaxFrames == 147, "ceil((1024 - 6) / 7) + 1");

IsoTpResult ignored(std::string reason) { return {IsoTpStatus::kIgnored, 0, {}, std::move(reason)}; }

}  // namespace

IsoTpResult IsoTpReassembler::push(std::span<const std::uint8_t> frame) {
  // Classic CAN: a frame carries at most 8 bytes.
  frame = frame.first(std::min<std::size_t>(frame.size(), 8));
  if (frame.empty()) return ignored("empty frame");
  if (frames_ >= kMaxFrames) {
    reset();
    return {IsoTpStatus::kAbandoned, 0, {}, "more than " + std::to_string(kMaxFrames) + " frames in one transfer"};
  }
  frames_ += 1;
  switch (frame[0] >> 4) {
    case kSingleFrame:
      return pushSingleFrame(frame);
    case kFirstFrame:
      return pushFirstFrame(frame);
    case kConsecutiveFrame:
      return pushConsecutiveFrame(frame);
    case kFlowControlFrame:
      return ignored("flow-control frame from the bus");
    default: {
      char reason[32];
      std::snprintf(reason, sizeof reason, "unknown PCI 0x%x", frame[0] >> 4);
      return ignored(reason);
    }
  }
}

void IsoTpReassembler::reset() {
  length_ = 0;
  filled_ = 0;
  expectedSequenceNumber_ = 1;
  frames_ = 0;
  receiving_ = false;
}

IsoTpResult IsoTpReassembler::pushSingleFrame(std::span<const std::uint8_t> frame) {
  const std::size_t length = frame[0] & 0x0f;
  if (length == 0) return ignored("single frame with zero length");
  if (frame.size() < 1 + length) {
    return ignored("single frame claims " + std::to_string(length) + " bytes but carries " +
                   std::to_string(frame.size() - 1));
  }
  reset();
  // A PCI length of 8-15 would need more than 8 bytes, which a classic frame can't carry.
  std::memcpy(single_.data(), frame.data() + 1, length);
  return {IsoTpStatus::kComplete, 0, std::span<const std::uint8_t>(single_.data(), length), {}};
}

IsoTpResult IsoTpReassembler::pushFirstFrame(std::span<const std::uint8_t> frame) {
  if (frame.size() < 8) return ignored("first frame shorter than 8 bytes");
  const std::size_t totalLength = (static_cast<std::size_t>(frame[0] & 0x0f) << 8) | frame[1];
  if (totalLength <= 6) return ignored("first frame declares only " + std::to_string(totalLength) + " bytes");
  if (totalLength > kMaxPayloadBytes) {
    reset();
    return {IsoTpStatus::kAbandoned, 0, {},
            "first frame declares " + std::to_string(totalLength) + " bytes, over the " +
                std::to_string(kMaxPayloadBytes) + " cap"};
  }
  // Each transfer starts with a fresh zeroed buffer.
  std::fill(payload_.begin(), payload_.begin() + totalLength, 0);
  std::memcpy(payload_.data(), frame.data() + 2, 6);
  length_ = totalLength;
  filled_ = 6;
  expectedSequenceNumber_ = 1;
  frames_ = 1;
  receiving_ = true;
  return {IsoTpStatus::kFlowControlRequired, totalLength, {}, {}};
}

IsoTpResult IsoTpReassembler::pushConsecutiveFrame(std::span<const std::uint8_t> frame) {
  if (!receiving_) return ignored("consecutive frame with no first frame");
  const std::uint8_t sequenceNumber = frame[0] & 0x0f;
  if (sequenceNumber != expectedSequenceNumber_) {
    const std::string reason = "consecutive frame out of sequence (expected " +
                               std::to_string(expectedSequenceNumber_) + ", got " + std::to_string(sequenceNumber) +
                               ")";
    reset();
    return {IsoTpStatus::kAbandoned, 0, {}, reason};
  }
  expectedSequenceNumber_ = (expectedSequenceNumber_ + 1) & 0x0f;
  const std::size_t remaining = length_ - filled_;
  const std::size_t available = frame.size() - 1;
  const std::size_t take = std::min({std::size_t{7}, remaining, available});
  std::memcpy(payload_.data() + filled_, frame.data() + 1, take);
  filled_ += take;
  if (filled_ < length_) return {IsoTpStatus::kIncomplete, 0, {}, {}};
  const std::size_t length = length_;
  reset();
  return {IsoTpStatus::kComplete, 0, std::span<const std::uint8_t>(payload_.data(), length), {}};
}

}  // namespace emcool::obd
