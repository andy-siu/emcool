#pragma once

// A console's `help`: one command per line, its usage then what it does, in two columns, so a
// telnet window or the dashboard's transcript reads it as a list rather than one long line.

#include <span>
#include <string>
#include <string_view>

namespace emcool::text {

struct HelpLine {
  std::string_view usage;  // "set NAME VALUE"; empty for a heading, which `what` then names
  std::string_view what;
};

// Where the second column starts. Usages are kept under it and descriptions short, so every
// line fits an 80-column terminal.
inline constexpr std::size_t kHelpColumn = 25;

// Appends `lines` to `output`, each ending in "\n": a heading flush left, a command indented.
void appendHelp(std::string& output, std::span<const HelpLine> lines);

}  // namespace emcool::text
