#include "emcool/ble/json_reader.hpp"

#include <cstdlib>
#include <cstring>

namespace emcool::ble {

namespace {

class Parser {
 public:
  Parser(std::string_view json, std::span<char> scratch) : json_(json), scratch_(scratch) {}

  bool atEnd() {
    skipWhitespace();
    return position_ == json_.size();
  }

  bool expect(char character) {
    skipWhitespace();
    if (position_ < json_.size() && json_[position_] == character) {
      position_++;
      return true;
    }
    return false;
  }

  bool peekIs(char character) {
    skipWhitespace();
    return position_ < json_.size() && json_[position_] == character;
  }

  // A string, unescaped into scratch. The opening quote must be next.
  bool string(std::string_view& out) {
    if (!expect('"')) return false;
    const std::size_t start = used_;
    while (position_ < json_.size()) {
      const char character = json_[position_++];
      if (character == '"') {
        out = std::string_view(scratch_.data() + start, used_ - start);
        return true;
      }
      if (static_cast<unsigned char>(character) < 0x20) return false;
      if (character != '\\') {
        if (!put(character)) return false;
        continue;
      }
      if (position_ >= json_.size()) return false;
      const char escape = json_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          if (!put(escape)) return false;
          break;
        case 'b':
          if (!put('\b')) return false;
          break;
        case 'f':
          if (!put('\f')) return false;
          break;
        case 'n':
          if (!put('\n')) return false;
          break;
        case 'r':
          if (!put('\r')) return false;
          break;
        case 't':
          if (!put('\t')) return false;
          break;
        case 'u':
          if (!unicodeEscape()) return false;
          break;
        default:
          return false;
      }
    }
    return false;
  }

  // Any value; members of nested containers are validated and not kept.
  bool value(JsonType& type, std::string_view& text, double& number, unsigned depth) {
    if (depth > JsonObjectReader::kMaxDepth) return false;
    skipWhitespace();
    if (position_ >= json_.size()) return false;
    const char first = json_[position_];
    if (first == '"') {
      type = JsonType::kString;
      return string(text);
    }
    const std::size_t start = position_;
    if (first == '{' || first == '[') {
      type = first == '{' ? JsonType::kObject : JsonType::kArray;
      if (!skipContainer(first, first == '{' ? '}' : ']', depth)) return false;
      // The container's raw text, for a caller that wants to read inside it in turn.
      text = json_.substr(start, position_ - start);
      return true;
    }
    if (literal("true") || literal("false")) {
      type = JsonType::kBool;
      text = json_.substr(start, position_ - start);
      return true;
    }
    if (literal("null")) {
      type = JsonType::kNull;
      text = json_.substr(start, position_ - start);
      return true;
    }
    type = JsonType::kNumber;
    return numberToken(text, number);
  }

 private:
  void skipWhitespace() {
    while (position_ < json_.size() &&
           (json_[position_] == ' ' || json_[position_] == '\t' || json_[position_] == '\n' || json_[position_] == '\r')) {
      position_++;
    }
  }

  bool put(char character) {
    if (used_ == scratch_.size()) return false;
    scratch_[used_++] = character;
    return true;
  }

  bool hexDigits(unsigned& codeUnit) {
    if (position_ + 4 > json_.size()) return false;
    codeUnit = 0;
    for (int index = 0; index < 4; index++) {
      const char digit = json_[position_++];
      codeUnit <<= 4;
      if (digit >= '0' && digit <= '9') {
        codeUnit |= static_cast<unsigned>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        codeUnit |= static_cast<unsigned>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        codeUnit |= static_cast<unsigned>(digit - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  // \uXXXX, with a surrogate pair combined, written out as UTF-8.
  bool unicodeEscape() {
    unsigned codePoint = 0;
    if (!hexDigits(codePoint)) return false;
    if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
      unsigned low = 0;
      if (position_ + 2 > json_.size() || json_[position_] != '\\' || json_[position_ + 1] != 'u') return false;
      position_ += 2;
      if (!hexDigits(low) || low < 0xdc00 || low > 0xdfff) return false;
      codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
    } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
      return false;
    }
    if (codePoint < 0x80) return put(static_cast<char>(codePoint));
    if (codePoint < 0x800) {
      return put(static_cast<char>(0xc0 | (codePoint >> 6))) && put(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    if (codePoint < 0x10000) {
      return put(static_cast<char>(0xe0 | (codePoint >> 12))) &&
             put(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f))) &&
             put(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    return put(static_cast<char>(0xf0 | (codePoint >> 18))) &&
           put(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f))) &&
           put(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f))) && put(static_cast<char>(0x80 | (codePoint & 0x3f)));
  }

  bool literal(std::string_view word) {
    if (json_.substr(position_, word.size()) != word) return false;
    position_ += word.size();
    return true;
  }

  bool digits() {
    const std::size_t start = position_;
    while (position_ < json_.size() && json_[position_] >= '0' && json_[position_] <= '9') {
      position_++;
    }
    return position_ > start;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  bool numberToken(std::string_view& text, double& number) {
    const std::size_t start = position_;
    if (position_ < json_.size() && json_[position_] == '-') position_++;
    if (position_ >= json_.size()) return false;
    if (json_[position_] == '0') {
      position_++;
    } else if (!digits()) {
      return false;
    }
    if (position_ < json_.size() && json_[position_] == '.') {
      position_++;
      if (!digits()) return false;
    }
    if (position_ < json_.size() && (json_[position_] == 'e' || json_[position_] == 'E')) {
      position_++;
      if (position_ < json_.size() && (json_[position_] == '+' || json_[position_] == '-')) position_++;
      if (!digits()) return false;
    }
    text = json_.substr(start, position_ - start);
    char token[40];
    if (text.size() >= sizeof token) return false;
    std::memcpy(token, text.data(), text.size());
    token[text.size()] = '\0';
    number = std::strtod(token, nullptr);
    return true;
  }

  bool skipContainer(char open, char close, unsigned depth) {
    if (!expect(open)) return false;
    if (expect(close)) return true;
    while (true) {
      if (open == '{') {
        std::string_view key;
        const std::size_t mark = used_;
        if (!string(key) || !expect(':')) return false;
        used_ = mark;
      }
      JsonType type;
      std::string_view text;
      double number;
      const std::size_t mark = used_;
      if (!value(type, text, number, depth + 1)) return false;
      used_ = mark;
      if (expect(close)) return true;
      if (!expect(',')) return false;
    }
  }

  std::string_view json_;
  std::span<char> scratch_;
  std::size_t position_ = 0;
  std::size_t used_ = 0;
};

}  // namespace

bool JsonObjectReader::read(std::string_view json, std::span<char> scratch) {
  count_ = 0;
  Parser parser(json, scratch);
  if (!parser.expect('{')) return false;
  if (!parser.expect('}')) {
    while (true) {
      JsonMember member = {};
      if (!parser.string(member.key) || !parser.expect(':')) return false;
      if (!parser.value(member.type, member.text, member.number, 1)) return false;
      if (count_ < kMaxMembers) {
        members_[count_++] = member;
      }
      if (parser.expect('}')) break;
      if (!parser.expect(',')) return false;
    }
  }
  return parser.atEnd();
}

const JsonMember* JsonObjectReader::find(std::string_view key) const {
  for (std::size_t index = 0; index < count_; index++) {
    if (members_[index].key == key) {
      return &members_[index];
    }
  }
  return nullptr;
}

bool jsonArrayElements(std::string_view array, std::vector<std::string_view>& elements, std::size_t limit) {
  elements.clear();
  std::size_t at = 0;
  const auto skipSpace = [&] {
    while (at < array.size() && (array[at] == ' ' || array[at] == '\t' || array[at] == '\n' || array[at] == '\r')) at++;
  };
  skipSpace();
  if (at >= array.size() || array[at] != '[') return false;
  at++;
  skipSpace();
  if (at < array.size() && array[at] == ']') return true;
  while (at < array.size()) {
    skipSpace();
    const std::size_t start = at;
    int depth = 0;
    bool inString = false;
    for (; at < array.size(); at++) {
      const char character = array[at];
      if (inString) {
        if (character == '\\') {
          at++;
        } else if (character == '"') {
          inString = false;
        }
        continue;
      }
      if (character == '"') {
        inString = true;
      } else if (character == '{' || character == '[') {
        depth++;
      } else if (character == '}' || character == ']') {
        if (depth == 0) break;
        depth--;
      } else if (character == ',' && depth == 0) {
        break;
      }
    }
    if (at >= array.size() || inString) return false;
    std::size_t end = at;
    while (end > start && (array[end - 1] == ' ' || array[end - 1] == '\n' || array[end - 1] == '\r' || array[end - 1] == '\t')) end--;
    if (end == start || elements.size() == limit) return false;
    elements.push_back(array.substr(start, end - start));
    if (array[at] == ']') return true;
    at++;  // the comma
  }
  return false;
}

}  // namespace emcool::ble
