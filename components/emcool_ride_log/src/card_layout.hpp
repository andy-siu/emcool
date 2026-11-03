#pragma once

// Where the ride log lives on the card, shared by the writer and the download:
// <root>/<boot, 5 digits>/PART<part, 4 digits>.LOG — 8.3 names, so FAT needs no long-filename
// entries.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emcool::ride_log::layout {

// The numbers of `directory`'s entries named <prefix><digits><suffix>, ascending, compared
// case-insensitively because FAT without long names hands names back upper-case. nullopt (and a
// warning) if the directory can't be listed.
std::optional<std::vector<std::uint32_t>> numberedEntries(const std::string& directory, std::string_view prefix,
                                                          std::string_view suffix);

inline constexpr std::string_view kPartPrefix = "PART";
inline constexpr std::string_view kPartSuffix = ".LOG";

std::string bootDirectory(const std::string& root, std::uint32_t boot);
std::string partPath(const std::string& root, std::uint32_t boot, std::uint32_t part);

}  // namespace emcool::ride_log::layout
