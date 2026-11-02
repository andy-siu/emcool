#include "emcool/text/text_buffer.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace emcool::text {

bool TextBuffer::reserve(std::size_t count) {
  if (overflowed_ || length_ + count > capacity_) {
    overflowed_ = true;
    return false;
  }
  return true;
}

void TextBuffer::append(std::string_view text) {
  if (windowed_) {
    const std::size_t start = total_;
    total_ += text.size();
    const std::size_t from = std::max(start, skip_);
    const std::size_t to = std::min(total_, skip_ + capacity_);
    if (from < to) {
      std::memcpy(data_ + (from - skip_), text.data() + (from - start), to - from);
      length_ = to - skip_;
    }
    return;
  }
  if (!reserve(text.size())) return;
  std::memcpy(data_ + length_, text.data(), text.size());
  length_ += text.size();
}

void TextBuffer::appendInteger(std::int64_t value) {
  char digits[24];
  const std::to_chars_result result = std::to_chars(digits, digits + sizeof digits, value);
  append(std::string_view(digits, static_cast<std::size_t>(result.ptr - digits)));
}

void TextBuffer::appendUnsigned(std::uint64_t value) {
  char digits[24];
  const std::to_chars_result result = std::to_chars(digits, digits + sizeof digits, value);
  append(std::string_view(digits, static_cast<std::size_t>(result.ptr - digits)));
}

// ECMAScript's Number::toString: the shortest digits that round-trip (s, k of them), placed by
// the decimal exponent n — plain up to 21 digits, "0.000…" down to 1e-7, exponential beyond.
void TextBuffer::appendNumber(double value) {
  if (value == 0) {
    append("0");  // and -0, as JSON.stringify writes it
    return;
  }
  char scientific[40];
  const std::to_chars_result result =
      std::to_chars(scientific, scientific + sizeof scientific, value, std::chars_format::scientific);
  if (result.ec != std::errc()) {
    overflowed_ = true;
    return;
  }
  const std::string_view text(scientific, static_cast<std::size_t>(result.ptr - scientific));
  const bool negative = text.front() == '-';
  const std::size_t mark = text.find('e');
  char digits[24];
  std::size_t k = 0;
  for (std::size_t at = negative ? 1 : 0; at < mark; at++) {
    if (text[at] != '.') digits[k++] = text[at];
  }
  int exponent = 0;
  std::from_chars(text.data() + mark + (text[mark + 1] == '+' ? 2 : 1), text.data() + text.size(), exponent);
  const int n = exponent + 1;
  const std::string_view s(digits, k);
  const int length = static_cast<int>(k);
  if (negative) append("-");
  if (length <= n && n <= 21) {
    append(s);
    for (int zero = 0; zero < n - length; zero++) append("0");
  } else if (0 < n && n <= 21) {
    append(s.substr(0, static_cast<std::size_t>(n)));
    append(".");
    append(s.substr(static_cast<std::size_t>(n)));
  } else if (-6 < n && n <= 0) {
    append("0.");
    for (int zero = 0; zero < -n; zero++) append("0");
    append(s);
  } else {
    append(s.substr(0, 1));
    if (length > 1) {
      append(".");
      append(s.substr(1));
    }
    append(n - 1 >= 0 ? "e+" : "e-");
    appendInteger(n - 1 >= 0 ? n - 1 : 1 - n);
  }
}

void TextBuffer::appendJsonString(std::string_view text) {
  append("\"");
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    const char* shortForm = nullptr;
    switch (character) {
      case '"':
        shortForm = "\\\"";
        break;
      case '\\':
        shortForm = "\\\\";
        break;
      case '\b':
        shortForm = "\\b";
        break;
      case '\f':
        shortForm = "\\f";
        break;
      case '\n':
        shortForm = "\\n";
        break;
      case '\r':
        shortForm = "\\r";
        break;
      case '\t':
        shortForm = "\\t";
        break;
      default:
        break;
    }
    if (shortForm != nullptr) {
      append(shortForm);
    } else if (byte < 0x20) {
      constexpr char kDigits[] = "0123456789abcdef";
      const char escaped[6] = {'\\', 'u', '0', '0', kDigits[byte >> 4], kDigits[byte & 0x0f]};
      append(std::string_view(escaped, 6));
    } else {
      append(std::string_view(&character, 1));
    }
  }
  append("\"");
}

}  // namespace emcool::text
