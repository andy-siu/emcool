#include "card_layout.hpp"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

#include "emcool/log.hpp"

namespace emcool::ride_log::layout {

namespace {

constexpr const char* kTag = "ride-log";

// The number in a name of the form <prefix><digits><suffix>. Anything else is not ours.
std::optional<std::uint32_t> numberIn(std::string_view name, std::string_view prefix, std::string_view suffix) {
  if (name.size() <= prefix.size() + suffix.size() || strncasecmp(name.data(), prefix.data(), prefix.size()) != 0 ||
      strncasecmp(name.data() + name.size() - suffix.size(), suffix.data(), suffix.size()) != 0) {
    return std::nullopt;
  }
  const std::string_view digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.size() > 9) return std::nullopt;
  std::uint32_t number = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') return std::nullopt;
    number = number * 10 + static_cast<std::uint32_t>(digit - '0');
  }
  return number;
}

}  // namespace

std::optional<std::vector<std::uint32_t>> numberedEntries(const std::string& directory, std::string_view prefix,
                                                          std::string_view suffix) {
  DIR* listing = opendir(directory.c_str());
  if (listing == nullptr) {
    logWarning(kTag, "cannot list %s (errno %d)", directory.c_str(), errno);
    return std::nullopt;
  }
  std::vector<std::uint32_t> numbers;
  while (const dirent* entry = readdir(listing)) {
    if (const std::optional<std::uint32_t> number = numberIn(entry->d_name, prefix, suffix)) {
      numbers.push_back(*number);
    }
  }
  closedir(listing);
  std::sort(numbers.begin(), numbers.end());
  return numbers;
}

std::string bootDirectory(const std::string& root, std::uint32_t boot) {
  char name[16];
  std::snprintf(name, sizeof name, "%05u", static_cast<unsigned>(boot));
  return root + "/" + name;
}

std::string partPath(const std::string& root, std::uint32_t boot, std::uint32_t part) {
  char name[16];
  std::snprintf(name, sizeof name, "PART%04u.LOG", static_cast<unsigned>(part));
  return bootDirectory(root, boot) + "/" + name;
}

}  // namespace emcool::ride_log::layout
