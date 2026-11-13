#include "emcool/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"

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

// Long enough for the BMS config warnings, the longest lines logged.
constexpr int kMessageCapacity = 768;

}  // namespace

void logInfo(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::info);
  char message[kMessageCapacity];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);
  ESP_LOGI(tag, "%s", message);
}

void logWarning(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::warning);
  char message[kMessageCapacity];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);
  ESP_LOGW(tag, "%s", message);
}

void logError(const char* tag, const char* format, ...) {
  count(tag, &LogCounts::error);
  char message[kMessageCapacity];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);
  ESP_LOGE(tag, "%s", message);
}

}  // namespace emcool
