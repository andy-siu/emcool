// TextBuffer writes numbers exactly as JavaScript does, so a body emcool builds can equal
// cool-eva's JSON.stringify byte for byte: tools/generate-golden-numbers.mts supplies the doubles
// and Node's String() of each. Also the JSON string escapes, and windows onto a longer text.
//
//   check_text <golden numbers>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "check.hpp"
#include "emcool/text/text_buffer.hpp"

using emcool::text::TextBuffer;

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_text <golden numbers>\n");
    return 2;
  }
  std::ifstream input(arguments[1]);
  std::string line;
  int total = 0;
  while (std::getline(input, line)) {
    const std::string hex = line.substr(0, 16);
    const std::string expected = line.substr(17);
    unsigned char bytes[8];
    for (int at = 0; at < 8; at++) bytes[at] = static_cast<unsigned char>(std::stoul(hex.substr(at * 2, 2), nullptr, 16));
    double value = 0;
    std::memcpy(&value, bytes, sizeof value);
    char out[64];
    TextBuffer text(out, sizeof out);
    text.appendNumber(value);
    const std::string mine(out, text.length());
    EMCOOL_CHECK(mine == expected, "%s: %s, JavaScript %s", hex.c_str(), mine.c_str(), expected.c_str());
    total++;
  }
  EMCOOL_CHECK(total > 100000, "only %d numbers in the golden file", total);

  // JSON.stringify's escapes.
  char out[64];
  TextBuffer text(out, sizeof out);
  text.appendJsonString("a\"b\\c\b\f\n\r\t\x01\x1f é");
  EMCOOL_CHECK(std::string(out, text.length()) == "\"a\\\"b\\\\c\\b\\f\\n\\r\\t\\u0001\\u001f é\"", "escapes: %.*s",
               static_cast<int>(text.length()), out);

  // A window keeps its stretch and counts the rest.
  const std::string whole = "0123456789abcdefghij";
  for (std::size_t skip = 0; skip <= whole.size() + 2; skip++) {
    char piece[7];
    TextBuffer window = TextBuffer::window(piece, sizeof piece, skip);
    window.append(whole.substr(0, 5));
    window.appendInteger(5678);
    window.append(whole.substr(9));
    const std::string expectedPiece = skip < whole.size() ? whole.substr(skip, sizeof piece) : "";
    EMCOOL_CHECK(window.total() == whole.size() && std::string(piece, window.length()) == expectedPiece,
                 "window at %zu: %.*s (total %zu)", skip, static_cast<int>(window.length()), piece, window.total());
  }

  std::printf("check_text: %d numbers written as JavaScript writes them; %d failures\n", total,
              emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
