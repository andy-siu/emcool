#include "emcool/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace emcool {

namespace {

struct TagCounts {
  const char* tag;
  LogCounts counts;
};

// A handful of tags log; the table only has to hold them.
constexpr int kMaxCountedTags = 32;
TagCounts tagCounts[kMaxCountedTags] = {};

LogCounts* countsFor(const char* tag) {
  for (TagCounts& entry : tagCounts) {
    if (entry.tag == nullptr) {
      entry.tag = tag;
      return &entry.counts;
    }
    if (entry.tag == tag || std::strcmp(entry.tag, tag) == 0) return &entry.counts;
  }
  return nullptr;
}

// Several tasks log at once.
std::mutex countsMutex;

void count(const char* tag, std::uint32_t LogCounts::*level) {
  const std::lock_guard<std::mutex> lock(countsMutex);
  if (LogCounts* counts = countsFor(tag)) (counts->*level)++;
}

}  // namespace

LogCounts logCountsFor(const char* tag) {
  const std::lock_guard<std::mutex> lock(countsMutex);
  const LogCounts* counts = countsFor(tag);
  return counts != nullptr ? *counts : LogCounts{};
}

namespace {

LogObserver observer = nullptr;
void* observerContext = nullptr;

void writeLine(char level, const char* tag, const char* format, va_list arguments) {
  char message[1024];
  std::vsnprintf(message, sizeof message, format, arguments);
  std::fprintf(stderr, "%c %s: %s\n", level, tag, message);
  if (observer != nullptr) observer(level, tag, message, observerContext);
}

}  // namespace

void setLogObserver(LogObserver newObserver, void* context) {
  observer = newObserver;
  observerContext = context;
}

void logInfo(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::info);
  va_list arguments;
  va_start(arguments, format);
  writeLine('I', tag, format, arguments);
  va_end(arguments);
}

void logWarning(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::warning);
  va_list arguments;
  va_start(arguments, format);
  writeLine('W', tag, format, arguments);
  va_end(arguments);
}

void logError(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::error);
  va_list arguments;
  va_start(arguments, format);
  writeLine('E', tag, format, arguments);
  va_end(arguments);
}

}  // namespace emcool
