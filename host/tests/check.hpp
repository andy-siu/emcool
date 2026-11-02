#pragma once

// A minimal check macro for the host tests: no framework to fetch, and a failure names the
// line it came from. Each test binary returns failures() from main.

#include <cstdio>

namespace emcool::test {

inline int& failures() {
  static int count = 0;
  return count;
}

}  // namespace emcool::test

#define EMCOOL_CHECK(condition, ...)                                         \
  do {                                                                       \
    if (!(condition)) {                                                      \
      ::emcool::test::failures()++;                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #condition); \
      std::fprintf(stderr, __VA_ARGS__);                                     \
      std::fputc('\n', stderr);                                              \
    }                                                                        \
  } while (0)
