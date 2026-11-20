#include "emcool/text/help.hpp"

namespace emcool::text {

void appendHelp(std::string& output, std::span<const HelpLine> lines) {
  for (const HelpLine& line : lines) {
    if (line.usage.empty()) {
      output.append(line.what);
      output += '\n';
      continue;
    }
    output += "  ";
    output.append(line.usage);
    const std::size_t used = 2 + line.usage.size();
    // A usage too long for its column still gets a gap before the description.
    output.append(used < kHelpColumn ? kHelpColumn - used : 2, ' ');
    output.append(line.what);
    output += '\n';
  }
}

}  // namespace emcool::text
