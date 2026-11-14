#pragma once

// The pure parts of the serial console's manual CAN commands (can-send, can-dump): parsing a
// typed frame and formatting a received one. No transport, so the host checks run them.
//
// Frames here are classic CAN, standard 11-bit ids, 0-8 data bytes — what the bus speaks.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace emcool::can {

// Standard 11-bit ids only.
inline constexpr std::uint32_t kMaxStandardId = 0x7ff;

struct ConsoleFrame {
  std::uint32_t id = 0;
  std::uint8_t length = 0;
  std::array<std::uint8_t, 8> data = {};
};

// Parses a can-send argument list (argv[0] is the command name, argv[1] the id, argv[2..] the
// data bytes, each two hex digits), each optionally with a 0x prefix. nullopt and a filled `out`
// on success; otherwise why it was refused.
std::optional<std::string> parseCanSend(int argumentCount, const char* const* arguments, ConsoleFrame& out);

// Parses can-dump's optional id filter (one hex id, 0x optional). nullopt with `out` left alone
// when there is no argument; the id when there is one; or an error string.
std::optional<std::string> parseCanDumpFilter(int argumentCount, const char* const* arguments,
                                              std::optional<std::uint32_t>& out);

// A received frame candump-style: the id (at least three hex digits), the length, then the bytes,
// e.g. "7DF [3] 02 01 0C". No trailing newline.
std::string formatCanFrame(std::uint32_t id, std::span<const std::uint8_t> data);

}  // namespace emcool::can
