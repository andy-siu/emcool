// The bike's memory for BigText (big_text.hpp): PSRAM first, then internal RAM.

#include "emcool/text/big_text.hpp"

#include "esp_heap_caps.h"

namespace emcool::text {

void* allocateBig(std::size_t bytes) {
  return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void releaseBig(void* memory) { heap_caps_free(memory); }

}  // namespace emcool::text
