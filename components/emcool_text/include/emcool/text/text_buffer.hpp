#pragma once

// Appends text into a fixed buffer without allocating. Running out of room sets a flag
// instead of truncating mid-value, so the caller can roll back to a length it saved.
//
// A window instead keeps only one stretch of a longer text, bytes [skip, skip + capacity),
// and counts the rest: writing the same text again with the next skip gives the next piece,
// so a body far bigger than any buffer can be sent a piece at a time.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace emcool::text {

class TextBuffer {
 public:
  TextBuffer(char* data, std::size_t capacity, std::size_t length = 0)
      : data_(data), capacity_(capacity), length_(length) {}
  static TextBuffer window(char* data, std::size_t capacity, std::size_t skip) {
    TextBuffer buffer(data, capacity);
    buffer.windowed_ = true;
    buffer.skip_ = skip;
    return buffer;
  }

  void append(std::string_view text);
  void appendInteger(std::int64_t value);
  void appendUnsigned(std::uint64_t value);
  // The shortest decimal that parses back to the same double, written exactly as JavaScript's
  // String(number) and JSON.stringify write it ("1e+21", "1e-7", "0.000001", "-0" as "0").
  // The caller guarantees it is finite.
  void appendNumber(double value);
  // Quoted and escaped as JSON.stringify does it: \" \\ \b \f \n \r \t, any other control
  // character as \u00xx, and all other UTF-8 as it is.
  void appendJsonString(std::string_view text);

  // The bytes held. For a window, those of its stretch that have been written so far.
  std::size_t length() const { return length_; }
  // A window's whole text so far, kept or not.
  std::size_t total() const { return total_; }
  bool overflowed() const { return overflowed_; }
  void truncate(std::size_t length) {
    length_ = length;
    overflowed_ = false;
  }

 private:
  bool reserve(std::size_t count);

  char* data_;
  std::size_t capacity_;
  std::size_t length_;
  bool overflowed_ = false;
  bool windowed_ = false;
  std::size_t skip_ = 0;
  std::size_t total_ = 0;
};

}  // namespace emcool::text
