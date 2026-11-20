// The laptop's memory for the SFTP server's buffers (sftp_server.hpp).

#include <cstdlib>

#include "emcool/files/sftp_server.hpp"

namespace emcool::files {

void* sftpAllocate(std::size_t bytes) { return std::malloc(bytes); }
void sftpRelease(void* memory) { std::free(memory); }

}  // namespace emcool::files
