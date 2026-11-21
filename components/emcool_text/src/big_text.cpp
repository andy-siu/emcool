#include "emcool/text/big_text.hpp"

#include <cstring>
#include <utility>

namespace emcool::text {

BigText BigText::uninitialised(std::size_t bytes) {
  BigText text;
  if (bytes == 0) return text;
  text.data_ = static_cast<char*>(allocateBig(bytes));
  if (text.data_ == nullptr) {
    text.failed_ = true;
    return text;
  }
  text.size_ = bytes;
  return text;
}

BigText::BigText(std::string_view text) : BigText(uninitialised(text.size())) {
  if (data_ != nullptr) std::memcpy(data_, text.data(), text.size());
}

BigText::BigText(BigText&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      failed_(std::exchange(other.failed_, false)) {}

BigText& BigText::operator=(BigText&& other) noexcept {
  if (this != &other) {
    releaseBig(data_);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    failed_ = std::exchange(other.failed_, false);
  }
  return *this;
}

}  // namespace emcool::text
