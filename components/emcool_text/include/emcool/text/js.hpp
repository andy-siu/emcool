#pragma once

// JavaScript's ways of turning numbers into text, for messages and bodies that must read exactly
// as JavaScript writes them.

#include <cmath>
#include <string>

#include "emcool/text/text_buffer.hpp"

namespace emcool::text {

// String(value).
inline std::string jsNumber(double value) {
  char text[40];
  TextBuffer out(text, sizeof text);
  out.appendNumber(value);
  return std::string(text, out.length());
}

// Math.round(value): halves go up, towards +∞.
inline double jsRound(double value) {
  const double floor = std::floor(value);
  return value - floor >= 0.5 ? floor + 1 : floor;
}

// value.toFixed(digits), for |value| < 1e21: the nearest, ties away from zero, judged on the
// double's exact decimal value — not printf's round-half-even.
std::string jsToFixed(double value, int digits);

}  // namespace emcool::text
