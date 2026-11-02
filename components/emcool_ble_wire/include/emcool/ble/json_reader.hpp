#pragma once

// Reads the small JSON headers the phone sends (HELLO, REQUEST). HELLO arrives before any
// authentication, so this treats its input as hostile: the whole document is checked against
// RFC 8259, nesting is bounded, and nothing is copied past the caller's buffer. Only the
// top-level members of an object come back; a nested value is validated and handed back as raw
// text. Also reads back the VCU snapshot rows emcool keeps on the card.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace emcool::ble {

enum class JsonType : std::uint8_t { kString, kNumber, kBool, kNull, kObject, kArray };

struct JsonMember {
  std::string_view key;
  JsonType type;
  // Unescaped, for strings; the literal token for numbers, booleans and null; the raw text, still
  // escaped, for an object or array — which another reader can then read.
  std::string_view text;
  double number;
};

// The elements of a JSON array, each as its raw text (a reader's member text for an array). False if
// it isn't an array, or more than `limit` elements. Only the nesting is checked; read each element in
// turn to check it.
bool jsonArrayElements(std::string_view array, std::vector<std::string_view>& elements, std::size_t limit = 4096);

class JsonObjectReader {
 public:
  // Enough for a VCU parameter row's 15.
  static constexpr std::size_t kMaxMembers = 16;
  static constexpr unsigned kMaxDepth = 16;

  // Parses `json`, unescaping keys and strings into `scratch`. False if it isn't a JSON
  // object, or the unescaped text doesn't fit; the members past kMaxMembers are checked and dropped.
  bool read(std::string_view json, std::span<char> scratch);

  const JsonMember* find(std::string_view key) const;
  std::span<const JsonMember> members() const { return {members_, count_}; }

 private:
  JsonMember members_[kMaxMembers] = {};
  std::size_t count_ = 0;
};

}  // namespace emcool::ble
