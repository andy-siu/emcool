#pragma once

// The pure parts of the serial console's manual CAN commands (can-send, can-dump): parsing a
// typed frame and formatting a received one. No transport, so the host checks run them.
//
// Frames here are classic CAN, 0-8 data bytes. An id is standard (11-bit) when typed with up to three
// hex digits and extended (29-bit) with four to eight, as SocketCAN's cansend reads them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace emcool::can {

inline constexpr std::uint32_t kMaxStandardId = 0x7ff;
inline constexpr std::uint32_t kMaxExtendedId = 0x1fffffff;

struct ConsoleFrame {
  std::uint32_t id = 0;
  bool extended = false;
  std::uint8_t length = 0;
  std::array<std::uint8_t, 8> data = {};
};

// How often `can-send -r` may repeat a frame: no faster than the 10 ms FreeRTOS tick, no slower than
// once a minute.
inline constexpr std::uint32_t kMinRepeatMs = 10;
inline constexpr std::uint32_t kMaxRepeatMs = 60000;

struct CanSendCommand {
  ConsoleFrame frame;
  // 0 sends the frame once; otherwise it is resent every this many milliseconds until stopped.
  std::uint32_t repeatMs = 0;
};

// Parses a can-send argument list: argv[0] is the command name, then an optional `-r <ms>` (resend
// every <ms> milliseconds, decimal), then the id and the data bytes, each two hex digits, each
// optionally with a 0x prefix. nullopt and a filled `out` on success; otherwise why it was refused.
std::optional<std::string> parseCanSend(int argumentCount, const char* const* arguments, CanSendCommand& out);

struct CanId {
  std::uint32_t id = 0;
  bool extended = false;
  bool operator==(const CanId&) const = default;
};

// Parses can-dump's optional id filter (one hex id, 0x optional, standard or extended by its
// length as can-send reads it). nullopt with `out` left alone when there is no argument; the id
// when there is one; or an error string.
std::optional<std::string> parseCanDumpFilter(int argumentCount, const char* const* arguments,
                                              std::optional<CanId>& out);

// A frame candump-style: the id (three hex digits for a standard id, eight for an extended one),
// the length, then the bytes, e.g. "7DF [3] 02 01 0C" or "18DAF1A0 [8] 02 7E 00 …". No trailing
// newline.
std::string formatCanFrame(std::uint32_t id, std::span<const std::uint8_t> data, bool extended = false);

}  // namespace emcool::can
