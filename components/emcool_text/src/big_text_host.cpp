// The laptop's memory for BigText (big_text.hpp). malloc, not operator new, so a host check that
// counts operator new counts what the bike would ask of its internal RAM, and not this.

#include <cstdlib>

#include "emcool/text/big_text.hpp"

namespace emcool::text {

void* allocateBig(std::size_t bytes) { return std::malloc(bytes); }
void releaseBig(void* memory) { std::free(memory); }

}  // namespace emcool::text
