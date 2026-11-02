#include "emcool/text/js.hpp"

#include <charconv>
#include <string_view>

namespace emcool::text {

std::string jsToFixed(double value, int digits) {
  const bool negative = value < 0;
  const double magnitude = negative ? -value : value;
  // Every double's decimal expansion ends within 1074 places, so this is exact.
  char exact[1200];
  const std::to_chars_result result =
      std::to_chars(exact, exact + sizeof exact, magnitude, std::chars_format::fixed, 1074);
  std::string text(exact, static_cast<std::size_t>(result.ptr - exact));
  const std::size_t point = text.find('.');
  std::string whole = text.substr(0, point);
  std::string fraction = point == std::string::npos ? std::string() : text.substr(point + 1);
  fraction.resize(static_cast<std::size_t>(digits) + 1, '0');
  // Round on the first dropped digit: anything at or past half goes up. The rest of the expansion
  // only matters below half, which a digit under 5 already is.
  const bool up = fraction[static_cast<std::size_t>(digits)] >= '5';
  std::string kept = whole + fraction.substr(0, static_cast<std::size_t>(digits));
  if (up) {
    std::size_t at = kept.size();
    while (at > 0) {
      at--;
      if (kept[at] == '9') {
        kept[at] = '0';
        continue;
      }
      kept[at]++;
      break;
    }
    if (at == 0 && kept[0] == '0') kept.insert(kept.begin(), '1');
  }
  const std::size_t wholeDigits = kept.size() - static_cast<std::size_t>(digits);
  std::string out = kept.substr(0, wholeDigits);
  if (digits > 0) out += "." + kept.substr(wholeDigits);
  // A negative value keeps its sign even when it rounds to zero, as toFixed's does; -0 is not < 0.
  return (negative ? "-" : "") + out;
}

}  // namespace emcool::text
