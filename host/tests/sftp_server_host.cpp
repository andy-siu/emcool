// The SSH console's SFTP server (emcool/files/sftp_server.hpp) on the laptop, speaking on stdin and
// stdout as OpenSSH's sftp-server does, so OpenSSH's own clients can drive it:
//
//   sftp -D "sftp_server_host CARD [RECORDING_BOOT [KEEP_FREE_BYTES]]"
//   scp -D "sftp_server_host CARD" FILE x:/FILE
//
// CARD is a folder standing in for the card. RECORDING_BOOT is the ride the ride log is writing
// (RIDES/<n>), 0 for none; KEEP_FREE_BYTES what an upload must leave free (the card's free space
// is the folder's file system's, from statvfs). The log goes to stderr.

#include <signal.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "emcool/files/sftp_server.hpp"

namespace {

class FolderCard final : public emcool::files::Card {
 public:
  FolderCard(std::string root, std::uint32_t recording) : root_(std::move(root)), recording_(recording) {}
  const std::string& mountPoint() const override { return root_; }
  std::mutex& lock() override { return mutex_; }
  emcool::files::CardState state() override { return {true, 1}; }
  emcool::files::CardFacts facts() override {
    emcool::files::CardFacts facts;
    if (recording_ != 0) facts.recordingBoot = recording_;
    return facts;
  }
  std::optional<emcool::files::FreeSpace> freeSpace() override {
    struct statvfs info = {};
    if (::statvfs(root_.c_str(), &info) != 0) return std::nullopt;
    return emcool::files::FreeSpace{std::uint64_t{info.f_bavail} * info.f_frsize,
                                    std::uint64_t{info.f_blocks} * info.f_frsize};
  }
  void changed(bool) override {}

 private:
  std::string root_;
  std::uint32_t recording_;
  std::mutex mutex_;
};

bool writeAll(std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t put = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
    if (put < 0 && errno == EINTR) continue;
    if (put <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(put));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr, "usage: sftp_server_host CARD [RECORDING_BOOT [KEEP_FREE_BYTES]]\n");
    return 2;
  }
  // A client that goes away is a write that fails, not a signal: the server is then destroyed, and
  // removes an unfinished upload as it does on the bike.
  ::signal(SIGPIPE, SIG_IGN);
  FolderCard card(argv[1], argc > 2 ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 0);
  emcool::files::CardShellGate gate;
  emcool::files::SftpServer server(card, gate, "SSH file transfer");
  server.setKeepFreeBytes(argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0);
  if (!server.ready()) return 1;
  char buffer[8192];
  while (true) {
    // Out first: the server answers only while it has room for an answer.
    if (!writeAll(server.pendingOutput())) return 1;
    server.sent(server.pendingOutput().size());
    if (!server.work()) return 1;
    if (!server.pendingOutput().empty()) continue;
    const std::size_t room = std::min(sizeof buffer, server.inputRoom());
    if (room == 0) return 1;  // a request that never fits
    const ssize_t got = ::read(STDIN_FILENO, buffer, room);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return 0;
    if (!server.receive(std::string_view(buffer, static_cast<std::size_t>(got)))) return 1;
  }
}
