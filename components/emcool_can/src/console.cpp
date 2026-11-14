#include "emcool/can/console.hpp"

#include <cstdio>
#include <string_view>

namespace emcool::can {

namespace {

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A hex number, 0x optional, up to `maxDigits` digits. false if it isn't one.
bool parseHex(std::string_view text, std::size_t maxDigits, std::uint32_t& out) {
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
  if (text.empty() || text.size() > maxDigits) return false;
  std::uint32_t value = 0;
  for (const char c : text) {
    const int digit = hexDigit(c);
    if (digit < 0) return false;
    value = (value << 4) | static_cast<std::uint32_t>(digit);
  }
  out = value;
  return true;
}

}  // namespace

std::optional<std::string> parseCanSend(int argumentCount, const char* const* arguments, ConsoleFrame& out) {
  if (argumentCount < 2) {
    return std::string("usage: can-send <id> [byte ...] — a standard 11-bit id (hex) and up to 8 data bytes (hex), "
                       "e.g. can-send 7DF 02 01 0C");
  }
  std::uint32_t id = 0;
  if (!parseHex(arguments[1], 8, id) || id > kMaxStandardId) {
    return "the id must be a hex standard 11-bit id, 0 to 7FF, e.g. 7DF — not \"" + std::string(arguments[1]) + "\"";
  }
  const int byteCount = argumentCount - 2;
  if (byteCount > 8) return std::string("at most 8 data bytes; a classic CAN frame carries no more");
  ConsoleFrame frame;
  frame.id = id;
  frame.length = static_cast<std::uint8_t>(byteCount);
  for (int at = 0; at < byteCount; at++) {
    std::uint32_t value = 0;
    if (!parseHex(arguments[at + 2], 2, value) || value > 0xff) {
      return "data byte " + std::to_string(at + 1) + " must be two hex digits, 00 to FF — not \"" +
             std::string(arguments[at + 2]) + "\"";
    }
    frame.data[static_cast<std::size_t>(at)] = static_cast<std::uint8_t>(value);
  }
  out = frame;
  return std::nullopt;
}

std::optional<std::string> parseCanDumpFilter(int argumentCount, const char* const* arguments,
                                              std::optional<std::uint32_t>& out) {
  if (argumentCount < 2) return std::nullopt;
  if (argumentCount > 2) return std::string("usage: can-dump [id] — one optional hex id to show only, e.g. can-dump 7E8");
  std::uint32_t id = 0;
  if (!parseHex(arguments[1], 8, id) || id > kMaxStandardId) {
    return "the id must be a hex standard 11-bit id, 0 to 7FF, e.g. 7E8 — not \"" + std::string(arguments[1]) + "\"";
  }
  out = id;
  return std::nullopt;
}

std::string formatCanFrame(std::uint32_t id, std::span<const std::uint8_t> data) {
  char head[24];
  std::snprintf(head, sizeof head, "%03X [%zu]", static_cast<unsigned>(id), data.size());
  std::string text = head;
  for (const std::uint8_t byte : data) {
    char pair[4];
    std::snprintf(pair, sizeof pair, " %02X", static_cast<unsigned>(byte));
    text += pair;
  }
  return text;
}

}  // namespace emcool::can
