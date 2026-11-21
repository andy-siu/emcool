#pragma once

// Text too big to trust to the bike's internal RAM: a reply body, a file read whole. Its bytes come
// from PSRAM where there is any (internal RAM if not), never from operator new, which on the bike
// only hands out internal RAM: about 13 KB free with Bluetooth and Wi-Fi up, and a block it can't
// find aborts the bike. Built measure-then-exact, so it is allocated once and never grows, and it is
// moved, never copied.
//
// When there's no memory for it at all, it is failed() and empty, for the caller to answer for.

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "emcool/text/text_buffer.hpp"

namespace emcool::text {

// PSRAM first, then internal RAM; nullptr if neither has `bytes`. One per platform:
// big_text_esp.cpp and big_text_host.cpp.
void* allocateBig(std::size_t bytes);
void releaseBig(void* memory);

class BigText {
 public:
  BigText() = default;
  // A copy of `text`, for the short fixed answers.
  explicit BigText(std::string_view text);
  BigText(BigText&& other) noexcept;
  BigText& operator=(BigText&& other) noexcept;
  BigText(const BigText&) = delete;
  BigText& operator=(const BigText&) = delete;
  ~BigText() { releaseBig(data_); }

  // What `write` writes, measured with a window first so the block is exactly its size.
  template <typename Write>
  static BigText build(Write&& write) {
    TextBuffer measure = TextBuffer::window(nullptr, 0, 0);
    write(measure);
    BigText text = uninitialised(measure.total());
    if (text.failed_) return text;
    TextBuffer out(text.data_, text.size_);
    write(out);
    text.size_ = out.length();
    return text;
  }

  // `bytes` bytes to fill through data(), then cut to what was filled with truncate().
  static BigText uninitialised(std::size_t bytes);
  char* data() { return data_; }
  void truncate(std::size_t size) { size_ = std::min(size, size_); }

  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
  std::string_view view() const { return {data_, size_}; }
  bool failed() const { return failed_; }

 private:
  char* data_ = nullptr;
  std::size_t size_ = 0;
  bool failed_ = false;
};

}  // namespace emcool::text
