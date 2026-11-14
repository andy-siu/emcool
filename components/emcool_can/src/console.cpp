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

// A decimal number of up to `maxDigits` digits. false if it isn't one.
bool parseDecimal(std::string_view text, std::size_t maxDigits, std::uint32_t& out) {
  if (text.empty() || text.size() > maxDigits) return false;
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
  }
  out = value;
  return true;
}

// An id as cansend reads it: up to three hex digits is standard (and must be at most 7FF), four to
// eight is extended (at most 1FFFFFFF). 0x optional, and not counted.
bool parseId(std::string_view text, std::uint32_t& id, bool& extended) {
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
  std::uint32_t value = 0;
  if (!parseHex(text, 8, value)) return false;
  const bool isExtended = text.size() > 3;
  if (value > (isExtended ? kMaxExtendedId : kMaxStandardId)) return false;
  id = value;
  extended = isExtended;
  return true;
}

const char* const kIdRule =
    "a hex id: up to 3 digits for a standard id (0 to 7FF, e.g. 7DF), 4 to 8 for an extended one "
    "(up to 1FFFFFFF, e.g. 18DAA0F1)";

}  // namespace

std::optional<std::string> parseCanSend(int argumentCount, const char* const* arguments, CanSendCommand& out) {
  const std::string usage =
      "usage: can-send [-r <ms>] <id> [byte ...] — a hex id (3 digits standard, 4-8 extended) and up to 8 "
      "data bytes (hex), e.g. can-send 7DF 02 01 0C; -r <ms> resends it every <ms> milliseconds until a key "
      "is pressed";
  std::uint32_t repeatMs = 0;
  int at = 1;
  if (argumentCount > at && std::string_view(arguments[at]) == "-r") {
    if (argumentCount <= at + 1) return usage;
    if (!parseDecimal(arguments[at + 1], 5, repeatMs) || repeatMs < kMinRepeatMs || repeatMs > kMaxRepeatMs) {
      return "-r takes a repeat interval in milliseconds, " + std::to_string(kMinRepeatMs) + " to " +
             std::to_string(kMaxRepeatMs) + " — not \"" + std::string(arguments[at + 1]) + "\"";
    }
    at += 2;
  }
  if (argumentCount <= at) return usage;
  std::uint32_t id = 0;
  bool extended = false;
  if (!parseId(arguments[at], id, extended)) {
    return std::string("the id must be ") + kIdRule + " — not \"" + std::string(arguments[at]) + "\"";
  }
  const int firstByte = at + 1;
  const int byteCount = argumentCount - firstByte;
  if (byteCount > 8) return std::string("at most 8 data bytes; a classic CAN frame carries no more");
  ConsoleFrame frame;
  frame.id = id;
  frame.extended = extended;
  frame.length = static_cast<std::uint8_t>(byteCount);
  for (int index = 0; index < byteCount; index++) {
    std::uint32_t value = 0;
    if (!parseHex(arguments[firstByte + index], 2, value) || value > 0xff) {
      return "data byte " + std::to_string(index + 1) + " must be two hex digits, 00 to FF — not \"" +
             std::string(arguments[firstByte + index]) + "\"";
    }
    frame.data[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
  }
  out.frame = frame;
  out.repeatMs = repeatMs;
  return std::nullopt;
}

std::optional<std::string> parseCanDumpFilter(int argumentCount, const char* const* arguments,
                                              std::optional<CanId>& out) {
  if (argumentCount < 2) return std::nullopt;
  if (argumentCount > 2) return std::string("usage: can-dump [id] — one optional hex id to show only, e.g. can-dump 7E8");
  CanId id;
  if (!parseId(arguments[1], id.id, id.extended)) {
    return std::string("the id must be ") + kIdRule + " — not \"" + std::string(arguments[1]) + "\"";
  }
  out = id;
  return std::nullopt;
}

std::string formatCanFrame(std::uint32_t id, std::span<const std::uint8_t> data, bool extended) {
  char head[24];
  std::snprintf(head, sizeof head, extended ? "%08X [%zu]" : "%03X [%zu]", static_cast<unsigned>(id), data.size());
  std::string text = head;
  for (const std::uint8_t byte : data) {
    char pair[4];
    std::snprintf(pair, sizeof pair, " %02X", static_cast<unsigned>(byte));
    text += pair;
  }
  return text;
}

}  // namespace emcool::can
