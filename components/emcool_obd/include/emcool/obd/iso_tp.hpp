#pragma once

// ISO 15765-2 (ISO-TP) receive-side reassembly, ported from cool-eva src/can/iso-tp.ts: the
// 39 stored trouble codes don't fit one frame, so the bike answers mode 03 as a first frame
// plus consecutive frames. Pure: frames in, a payload out; sending the flow control the first
// frame asks for is the caller's job.
//
// Each push() says what happened. A transfer that goes wrong is abandoned and reset, rather
// than handing back a payload with a hole in it; a frame that can't belong to any transfer
// is ignored and left the state alone. As in cool-eva, every push counts towards the frame
// cap, including ones that are ignored.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace emcool::obd {

enum class IsoTpStatus : std::uint8_t { kIncomplete, kFlowControlRequired, kComplete, kIgnored, kAbandoned };

struct IsoTpResult {
  IsoTpStatus status;
  // kFlowControlRequired: the length the first frame declared.
  std::size_t totalLength = 0;
  // kComplete: the payload, valid until the next push().
  std::span<const std::uint8_t> payload;
  // kIgnored and kAbandoned: why.
  std::string reason;
};

class IsoTpReassembler {
 public:
  // cool-eva's cap: 1 KB is far above any reply this bike gives, and keeps a malicious or
  // corrupted first frame from claiming 4 KB.
  static constexpr std::size_t kMaxPayloadBytes = 1024;
  static constexpr std::size_t kMaxFrames = (kMaxPayloadBytes - 6 + 6) / 7 + 1;

  IsoTpResult push(std::span<const std::uint8_t> frame);
  void reset();

 private:
  IsoTpResult pushSingleFrame(std::span<const std::uint8_t> frame);
  IsoTpResult pushFirstFrame(std::span<const std::uint8_t> frame);
  IsoTpResult pushConsecutiveFrame(std::span<const std::uint8_t> frame);

  std::array<std::uint8_t, kMaxPayloadBytes> payload_ = {};
  // The length the first frame declared; 0 outside a transfer.
  std::size_t length_ = 0;
  std::size_t filled_ = 0;
  std::uint8_t expectedSequenceNumber_ = 1;
  std::size_t frames_ = 0;
  bool receiving_ = false;
  // A completed single frame's payload, so it outlives the caller's frame buffer.
  std::array<std::uint8_t, 7> single_ = {};
};

}  // namespace emcool::obd
