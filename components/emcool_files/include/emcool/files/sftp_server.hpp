#pragma once

// An SFTP server (protocol version 3, what OpenSSH's sftp and scp and the GUI clients speak) for
// the microSD card, over the SSH console's channel. It sees only bytes in and bytes out, and the
// card through files::Card, so the host checks drive it with OpenSSH's own sftp and scp.
//
// The card's rules are the card commands' (card_shell.hpp), checked again before every change:
//   - nothing in the ride being recorded is removed, renamed or written, nothing new goes in RIDES,
//     and vcu-params is read-only (cardChangeRefusal);
//   - one console changes the card at a time: an upload, remove, rename, mkdir, rmdir or a file's
//     time takes the consoles' gate, and an upload keeps it until its file is closed;
//   - an upload stops before it would leave less than kKeepFreeBytes free for the ride log;
//   - an upload goes in under a temporary name (NAME.part) and takes its real name only once it's
//     closed whole, replacing what had the name then. A dropped connection or a full card leaves the
//     old file as it was. So a file is only ever written whole: what an upload writes is the whole
//     new file, opened with TRUNCATE or not (scp leaves it off), and appending is refused.
// Reading is always allowed, the ride being recorded included.
//
// Every file operation holds the card's lock and checks it is still the mount the file was opened
// on; a descriptor from before an unmount is forgotten, never closed.
//
// / is the card's top folder. FAT has no links, owners or permissions: a client's permissions are
// accepted and ignored, links are unsupported. OpenSSH's clients print only a status's kind
// ("Permission denied", "Failure"), not its message; the log says why.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "emcool/files/card_shell.hpp"

namespace emcool::files {

// Memory for the session's two big buffers: PSRAM first on the bike (sftp_memory_esp.cpp), where
// internal RAM has no 32 KB block to spare; malloc on the laptop.
void* sftpAllocate(std::size_t bytes);
void sftpRelease(void* memory);

class SftpServer {
 public:
  // At most this many files and folders open at once.
  static constexpr std::size_t kMaxHandles = 4;
  // The most one READ answers with, and the most one WRITE may carry: what OpenSSH asks for.
  static constexpr std::uint32_t kMaxDataBytes = 32 * 1024;
  // A longer request ends the session; none of OpenSSH's is.
  static constexpr std::uint32_t kMaxRequestBytes = kMaxDataBytes + 1024;
  // The most one answer takes: a request is answered only once there's this much room for it.
  static constexpr std::size_t kReplyRoom = kMaxDataBytes + 1024;
  // How many names one READDIR answers with.
  static constexpr std::size_t kNamesPerReply = 16;
  // The card is read and written this much at a time, through a buffer in internal RAM: FatFs
  // sends a PSRAM buffer to the card a sector at a time through a bounce buffer.
  static constexpr std::size_t kStagingBytes = 4096;

  // `console` names it to the other consoles ("SSH file transfer").
  SftpServer(Card& card, CardShellGate& gate, std::string console);
  SftpServer(const SftpServer&) = delete;
  SftpServer& operator=(const SftpServer&) = delete;
  // Closes what's still open; an unfinished upload is removed.
  ~SftpServer();

  // False when there was no memory for the buffers: the session can't start.
  bool ready() const { return input_.capacity() > 0 && output_.capacity() > 0; }

  // How many more bytes receive() takes now.
  std::size_t inputRoom() const { return input_.room(); }
  // Bytes from the client, at most inputRoom(); answers what it can. False when the session must
  // end (a malformed request).
  bool receive(std::string_view bytes);
  // Answers more of what's waiting, once output has gone out. False as receive().
  bool work();

  // What's ready to go to the client, and that `bytes` of it have gone.
  std::string_view pendingOutput() const { return output_.view(); }
  void sent(std::size_t bytes) { output_.consume(bytes); }

  // For the host checks.
  void setKeepFreeBytes(std::uint64_t bytes) { keepFreeBytes_ = bytes; }

 private:
  // A fixed-size byte buffer, allocated once.
  class Bytes {
   public:
    explicit Bytes(std::size_t capacity);
    ~Bytes();
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return end_ - begin_; }
    std::size_t room() const { return capacity_ - size(); }
    std::string_view view() const { return {data_ + begin_, size()}; }
    // `count` more bytes at the end, uninitialised; nullptr when they don't fit. Offsets from the
    // front stay valid across it; pointers don't.
    char* grow(std::size_t count);
    void shrinkTo(std::size_t size) { end_ = begin_ + size; }
    void consume(std::size_t count);
    char* at(std::size_t offset) { return data_ + begin_ + offset; }

   private:
    char* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
  };

  using Path = std::vector<std::string>;
  struct Attributes {
    bool exists = false;
    bool folder = false;
    std::uint64_t bytes = 0;
    std::uint32_t modified = 0;
  };
  struct Handle {
    bool used = false;
    std::uint32_t id = 0;
    std::uint32_t generation = 0;
    Path path;
    // A folder being listed (a DIR*), or nullptr.
    void* folder = nullptr;
    // A file being read, or an upload's temporary file; -1 once forgotten.
    int fd = -1;
    // An upload: written under `temporary`, renamed to `path` when it's closed.
    bool upload = false;
    Path temporary;
    bool failed = false;
    // Bytes written (an upload) or sent (a file being read).
    std::uint64_t bytes = 0;
    std::optional<std::uint32_t> modified;
  };
  class Reader;

  void answer(std::uint8_t type, std::uint32_t id, Reader& body);
  void doInit(Reader& body);
  void doExtended(std::uint32_t id, Reader& body);
  void doRealPath(std::uint32_t id, Reader& body);
  void doStat(std::uint32_t id, Reader& body);
  void doFstat(std::uint32_t id, Reader& body);
  void doSetStat(std::uint32_t id, Reader& body);
  void doFsetStat(std::uint32_t id, Reader& body);
  void doOpen(std::uint32_t id, Reader& body);
  void doOpenFolder(std::uint32_t id, Reader& body);
  void doReadFolder(std::uint32_t id, Reader& body);
  void doRead(std::uint32_t id, Reader& body);
  void doWrite(std::uint32_t id, Reader& body);
  void doClose(std::uint32_t id, Reader& body);
  void doRemove(std::uint32_t id, Reader& body);
  void doMakeFolder(std::uint32_t id, Reader& body);
  void doRemoveFolder(std::uint32_t id, Reader& body);
  void doRename(std::uint32_t id, Reader& body);
  void openUpload(std::uint32_t id, const Path& path, std::uint32_t flags, std::optional<std::uint32_t> modified);
  void closeUpload(std::uint32_t id, Handle& upload);
  void setTime(std::uint32_t id, const Path& path, std::uint32_t modified);

  // A client's path, as names from the card's top. False for one holding a \, a : or a NUL.
  static bool parsePath(std::string_view text, Path& path);
  std::string hostPath(const Path& path) const;
  static std::string shown(const Path& path);
  Handle* findHandle(std::string_view handle);
  Handle* newHandle();
  // Whether a handle has `path` open, or something inside it.
  bool isOpen(const Path& path) const;
  // Closes the handle's file or folder (when it's still the card it was opened on), and removes an
  // unfinished upload's temporary file.
  void release(Handle& handle);
  char* staging();

  // Runs `operation` under the card's lock if the card is mounted and, when `generation` is given,
  // still the mount it names. The mount's generation goes to `now` when it isn't null.
  template <typename Operation>
  bool underLock(const std::optional<std::uint32_t>& generation, std::uint32_t* now, Operation&& operation);
  // False when the card isn't mounted.
  bool statPath(const Path& path, Attributes& attributes, int& error);
  // Whether `path` may be changed now; when it mayn't, answers `id` and logs why.
  bool allowed(std::uint32_t id, const Path& path, bool creating, const char* what);
  // The consoles' gate, for a change; when another console has it, answers `id`.
  bool takeGate(std::uint32_t id);
  // Lets the gate go, unless an upload still needs it.
  void giveGate();
  void changed(const Path& path);

  // Replies, built straight into output_.
  std::size_t beginReply(std::uint8_t type, std::optional<std::uint32_t> id);
  void endReply(std::size_t start);
  void put(const void* bytes, std::size_t count);
  void putU32(std::uint32_t value);
  void putU64(std::uint64_t value);
  void putString(std::string_view text);
  void putAttributes(const Attributes& attributes);
  void sendStatus(std::uint32_t id, std::uint32_t code, std::string_view message);
  void sendError(std::uint32_t id, int error, const std::string& what);
  void sendBadMessage(std::uint32_t id);
  void sendNoHandle(std::uint32_t id);
  void sendCardGone(std::uint32_t id);
  void sendHandle(std::uint32_t id, const Handle& handle);
  static std::string longName(std::string_view name, const Attributes& attributes);

  Card& card_;
  CardShellGate& gate_;
  std::string console_;
  std::uint64_t keepFreeBytes_ = CardShell::kKeepFreeBytes;
  Bytes input_;
  Bytes output_;
  std::unique_ptr<char[]> staging_;
  bool stagingTried_ = false;
  bool overflowed_ = false;
  bool initialized_ = false;
  bool holdsGate_ = false;
  Handle handles_[kMaxHandles];
  std::uint32_t nextHandleId_ = 1;
};

}  // namespace emcool::files
