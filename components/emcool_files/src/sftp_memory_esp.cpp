// The bike's memory for the SFTP server's buffers (sftp_server.hpp): PSRAM first. With Wi-Fi and
// Bluetooth up, internal RAM has no 32 KB block to spare; the buffers only pass bytes between the
// channel and a small internal staging buffer, so PSRAM's speed doesn't matter.

#include "emcool/files/sftp_server.hpp"
#include "esp_heap_caps.h"

namespace emcool::files {

void* sftpAllocate(std::size_t bytes) {
  return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}
void sftpRelease(void* memory) { heap_caps_free(memory); }

}  // namespace emcool::files
