#include "emcool/files/sftp_server.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>

#include "emcool/log.hpp"

namespace emcool::files {

namespace {

constexpr const char* kTag = "sftp";
constexpr const char* kRides = "RIDES";
constexpr const char* kPartSuffix = ".part";
constexpr std::uint32_t kProtocolVersion = 3;

// Packet types (draft-ietf-secsh-filexfer-02: version 3).
enum : std::uint8_t {
  kInit = 1,
  kVersion = 2,
  kOpen = 3,
  kClose = 4,
  kRead = 5,
  kWrite = 6,
  kLstat = 7,
  kFstat = 8,
  kSetStat = 9,
  kFsetStat = 10,
  kOpenDir = 11,
  kReadDir = 12,
  kRemove = 13,
  kMkdir = 14,
  kRmdir = 15,
  kRealPath = 16,
  kStat = 17,
  kRename = 18,
  kStatus = 101,
  kHandle = 102,
  kData = 103,
  kName = 104,
  kAttrs = 105,
  kExtended = 200,
  kExtendedReply = 201,
};

// Status codes.
enum : std::uint32_t {
  kOk = 0,
  kEof = 1,
  kNoSuchFile = 2,
  kPermissionDenied = 3,
  kFailure = 4,
  kBadMessage = 5,
  kUnsupported = 8,
};

// Attribute flags.
constexpr std::uint32_t kAttrSize = 0x1;
constexpr std::uint32_t kAttrUidGid = 0x2;
constexpr std::uint32_t kAttrPermissions = 0x4;
constexpr std::uint32_t kAttrTimes = 0x8;
constexpr std::uint32_t kAttrExtended = 0x80000000u;

// OPEN's flags.
constexpr std::uint32_t kOpenWrite = 0x2;
constexpr std::uint32_t kOpenAppend = 0x4;
constexpr std::uint32_t kOpenCreate = 0x8;
constexpr std::uint32_t kOpenExclusive = 0x20;

constexpr std::uint32_t kFileMode = 0100644;
constexpr std::uint32_t kFolderMode = 040755;
// The largest offset the card's file calls take: off_t is 32 bits on the ESP32.
constexpr std::uint64_t kMaxOffset = INT32_MAX;

std::uint32_t readU32(const char* bytes) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes);
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

void writeU32(char* bytes, std::uint32_t value) {
  bytes[0] = static_cast<char>(value >> 24);
  bytes[1] = static_cast<char>(value >> 16);
  bytes[2] = static_cast<char>(value >> 8);
  bytes[3] = static_cast<char>(value);
}

// A name as FatFs takes it: trailing spaces and dots dropped.
std::string_view fatName(std::string_view name) {
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.remove_suffix(1);
  return name;
}

// The same entry on the card: FAT's names ignore case (ASCII is all code page 437 folds).
bool sameName(std::string_view left, std::string_view right) {
  left = fatName(left);
  right = fatName(right);
  if (left.size() != right.size()) return false;
  for (std::size_t at = 0; at < left.size(); at++) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lower(left[at]) != lower(right[at])) return false;
  }
  return true;
}

// Whether `inner` is `outer` or inside it.
bool within(const std::vector<std::string>& inner, const std::vector<std::string>& outer) {
  if (outer.size() > inner.size()) return false;
  for (std::size_t at = 0; at < outer.size(); at++) {
    if (!sameName(inner[at], outer[at])) return false;
  }
  return true;
}

bool samePath(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  return left.size() == right.size() && within(left, right);
}

std::uint32_t statusFor(int error) {
  switch (error) {
    case ENOENT:
    case ENOTDIR:
      return kNoSuchFile;
    case EACCES:
    case EPERM:
      return kPermissionDenied;
    default:
      return kFailure;
  }
}

std::string sizeText(std::uint64_t bytes) {
  char text[32];
  if (bytes >= 10ull * 1024 * 1024) {
    std::snprintf(text, sizeof text, "%llu MB", static_cast<unsigned long long>(bytes / (1024 * 1024)));
  } else if (bytes >= 10ull * 1024) {
    std::snprintf(text, sizeof text, "%llu KB", static_cast<unsigned long long>(bytes / 1024));
  } else {
    std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
  }
  return text;
}

}  // namespace

// --------------------------------------------------------------------------------- reading

// A request's fields, in order; any that runs past the end marks the request unreadable.
class SftpServer::Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  bool ok() const { return ok_; }

  std::uint32_t u32() {
    if (!take(4)) return 0;
    return readU32(data_.data() + at_ - 4);
  }
  std::uint64_t u64() {
    const std::uint64_t high = u32();
    return (high << 32) | u32();
  }
  std::string_view string() {
    const std::uint32_t length = u32();
    if (!take(length)) return {};
    return data_.substr(at_ - length, length);
  }

  // What a request's attributes set, and the two this server uses.
  struct Requested {
    std::uint32_t flags = 0;
    std::uint64_t size = 0;
    std::uint32_t modified = 0;
  };
  Requested attributes() {
    Requested requested;
    requested.flags = u32();
    if (requested.flags & kAttrSize) requested.size = u64();
    if (requested.flags & kAttrUidGid) {
      u32();
      u32();
    }
    if (requested.flags & kAttrPermissions) u32();
    if (requested.flags & kAttrTimes) {
      u32();
      requested.modified = u32();
    }
    if (requested.flags & kAttrExtended) {
      const std::uint32_t count = u32();
      for (std::uint32_t at = 0; at < count && ok_; at++) {
        string();
        string();
      }
    }
    return requested;
  }

 private:
  bool take(std::size_t count) {
    if (!ok_ || data_.size() - at_ < count) {
      ok_ = false;
      return false;
    }
    at_ += count;
    return true;
  }

  std::string_view data_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

// ----------------------------------------------------------------------------------- buffers

SftpServer::Bytes::Bytes(std::size_t capacity)
    : data_(static_cast<char*>(sftpAllocate(capacity))), capacity_(data_ != nullptr ? capacity : 0) {}

SftpServer::Bytes::~Bytes() { sftpRelease(data_); }

char* SftpServer::Bytes::grow(std::size_t count) {
  if (count > room()) return nullptr;
  if (end_ + count > capacity_) {
    std::memmove(data_, data_ + begin_, size());
    end_ -= begin_;
    begin_ = 0;
  }
  char* at = data_ + end_;
  end_ += count;
  return at;
}

void SftpServer::Bytes::consume(std::size_t count) {
  begin_ += std::min(count, size());
  if (begin_ == end_) begin_ = end_ = 0;
}

// ------------------------------------------------------------------------------------ session

SftpServer::SftpServer(Card& card, CardShellGate& gate, std::string console)
    : card_(card), gate_(gate), console_(std::move(console)), input_(kMaxRequestBytes + 4), output_(2 * kReplyRoom) {}

SftpServer::~SftpServer() {
  for (Handle& handle : handles_) {
    if (handle.used) release(handle);
  }
  if (holdsGate_) gate_.give(console_);
}

bool SftpServer::receive(std::string_view bytes) {
  char* into = input_.grow(bytes.size());
  if (into == nullptr) {
    logError(kTag, "more came from the client than there was room for");
    return false;
  }
  std::memcpy(into, bytes.data(), bytes.size());
  return work();
}

bool SftpServer::work() {
  while (input_.size() >= 4) {
    const std::uint32_t length = readU32(input_.at(0));
    if (length == 0 || length > kMaxRequestBytes) {
      logWarning(kTag, "a request of %u bytes, more than the %u this server takes; ending the session",
                 static_cast<unsigned>(length), static_cast<unsigned>(kMaxRequestBytes));
      return false;
    }
    if (input_.size() < 4 + std::size_t{length}) return true;
    if (output_.room() < kReplyRoom) return true;
    const std::string_view packet(input_.at(4), length);
    const auto type = static_cast<std::uint8_t>(packet[0]);
    if (type == kInit) {
      Reader body(packet.substr(1));
      doInit(body);
    } else {
      if (!initialized_ || length < 5) {
        logWarning(kTag, "a request before the session began; ending it");
        return false;
      }
      const std::uint32_t id = readU32(packet.data() + 1);
      Reader body(packet.substr(5));
      answer(type, id, body);
    }
    input_.consume(4 + std::size_t{length});
    if (overflowed_) {
      logError(kTag, "an answer was longer than its room; ending the session");
      return false;
    }
  }
  return true;
}

void SftpServer::answer(std::uint8_t type, std::uint32_t id, Reader& body) {
  switch (type) {
    case kOpen:
      return doOpen(id, body);
    case kClose:
      return doClose(id, body);
    case kRead:
      return doRead(id, body);
    case kWrite:
      return doWrite(id, body);
    case kLstat:
    case kStat:
      return doStat(id, body);
    case kFstat:
      return doFstat(id, body);
    case kSetStat:
      return doSetStat(id, body);
    case kFsetStat:
      return doFsetStat(id, body);
    case kOpenDir:
      return doOpenFolder(id, body);
    case kReadDir:
      return doReadFolder(id, body);
    case kRemove:
      return doRemove(id, body);
    case kMkdir:
      return doMakeFolder(id, body);
    case kRmdir:
      return doRemoveFolder(id, body);
    case kRealPath:
      return doRealPath(id, body);
    case kRename:
      return doRename(id, body);
    case kExtended:
      return doExtended(id, body);
    default:
      // Links (READLINK, SYMLINK): FAT has none.
      return sendStatus(id, kUnsupported, "not something the card does");
  }
}

void SftpServer::doInit(Reader& body) {
  body.u32();  // the client's version; 3 is what this answers to all of them
  initialized_ = true;
  const std::size_t start = beginReply(kVersion, std::nullopt);
  putU32(kProtocolVersion);
  putString("limits@openssh.com");
  putString("1");
  putString("statvfs@openssh.com");
  putString("2");
  endReply(start);
}

void SftpServer::doExtended(std::uint32_t id, Reader& body) {
  const std::string_view request = body.string();
  if (!body.ok()) return sendBadMessage(id);
  if (request == "limits@openssh.com") {
    // What the client may ask of this server: the sizes it then reads and writes in.
    const std::size_t start = beginReply(kExtendedReply, id);
    putU64(kMaxRequestBytes);
    putU64(kMaxDataBytes);
    putU64(kMaxDataBytes);
    putU64(kMaxHandles);
    endReply(start);
    return;
  }
  if (request == "statvfs@openssh.com") {
    // sftp's df.
    const std::optional<FreeSpace> space = card_.freeSpace();
    if (!space) return sendCardGone(id);
    constexpr std::uint64_t kBlock = 4096;
    const std::size_t start = beginReply(kExtendedReply, id);
    putU64(kBlock);                       // f_bsize
    putU64(kBlock);                       // f_frsize
    putU64(space->totalBytes / kBlock);   // f_blocks
    putU64(space->freeBytes / kBlock);    // f_bfree
    putU64(space->freeBytes / kBlock);    // f_bavail
    putU64(0);                            // f_files
    putU64(0);                            // f_ffree
    putU64(0);                            // f_favail
    putU64(0);                            // f_fsid
    putU64(0);                            // f_flag
    putU64(255);                          // f_namemax
    endReply(start);
    return;
  }
  sendStatus(id, kUnsupported, "not an extension this server has");
}

// ------------------------------------------------------------------------------- the requests

void SftpServer::doRealPath(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  const std::string name = shown(path);
  const std::size_t start = beginReply(kName, id);
  putU32(1);
  putString(name);
  putString(name);
  putU32(0);  // no attributes
  endReply(start);
}

void SftpServer::doStat(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  Attributes attributes;
  int error = 0;
  if (!statPath(path, attributes, error)) return sendCardGone(id);
  if (!attributes.exists) return sendError(id, error, shown(path));
  const std::size_t start = beginReply(kAttrs, id);
  putAttributes(attributes);
  endReply(start);
}

void SftpServer::doFstat(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  Attributes attributes;
  int error = 0;
  if (!statPath(handle->upload ? handle->temporary : handle->path, attributes, error)) return sendCardGone(id);
  if (!attributes.exists) return sendError(id, error, shown(handle->path));
  // An open file's size on the card is only brought up to date when it's closed.
  if (handle->upload) attributes.bytes = std::max(attributes.bytes, handle->bytes);
  const std::size_t start = beginReply(kAttrs, id);
  putAttributes(attributes);
  endReply(start);
}

void SftpServer::doSetStat(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  const Reader::Requested requested = body.attributes();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  if (requested.flags & kAttrSize) return sendStatus(id, kUnsupported, "a file on the card can't be cut short or grown");
  // Permissions and owners: FAT has neither, and nothing to set. The top folder has no time.
  if (!(requested.flags & kAttrTimes) || path.empty()) return sendStatus(id, kOk, "");
  setTime(id, path, requested.modified);
}

void SftpServer::doFsetStat(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  const Reader::Requested requested = body.attributes();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  if (requested.flags & kAttrSize) {
    // scp sets the size it wrote once it's done; an upload's temporary file may be cut to it.
    if (!handle->upload || requested.size > handle->bytes) {
      return sendStatus(id, kUnsupported, "a file on the card can't be cut short or grown");
    }
    if (requested.size < handle->bytes) {
      if (handle->failed || handle->fd < 0) return sendStatus(id, kFailure, "an earlier write to that file failed");
      int error = 0;
      if (!underLock(handle->generation, nullptr, [&] {
            if (::ftruncate(handle->fd, static_cast<off_t>(requested.size)) != 0) error = errno;
          })) {
        handle->fd = -1;
        handle->failed = true;
        return sendCardGone(id);
      }
      if (error != 0) {
        handle->failed = true;
        return sendError(id, error, shown(handle->path));
      }
      handle->bytes = requested.size;
    }
  }
  if (!(requested.flags & kAttrTimes)) return sendStatus(id, kOk, "");
  if (handle->upload) {
    // scp -p and sftp put -p: set once the file has its name.
    handle->modified = requested.modified;
    return sendStatus(id, kOk, "");
  }
  setTime(id, handle->path, requested.modified);
}

void SftpServer::setTime(std::uint32_t id, const Path& path, std::uint32_t modified) {
  if (!takeGate(id)) return;
  if (!allowed(id, path, false, "setting the time of")) return giveGate();
  if (isOpen(path)) {
    giveGate();
    return sendStatus(id, kFailure, "that file is being written");
  }
  const std::string host = hostPath(path);
  int error = 0;
  const bool locked = underLock(std::nullopt, nullptr, [&] {
    utimbuf times = {};
    times.actime = static_cast<std::time_t>(modified);
    times.modtime = static_cast<std::time_t>(modified);
    if (::utime(host.c_str(), &times) != 0) error = errno;
  });
  giveGate();
  if (!locked) return sendCardGone(id);
  if (error != 0) return sendError(id, error, shown(path));
  sendStatus(id, kOk, "");
}

void SftpServer::doOpen(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  const std::uint32_t flags = body.u32();
  const Reader::Requested requested = body.attributes();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  if (flags & (kOpenWrite | kOpenAppend)) {
    return openUpload(id, path, flags,
                      (requested.flags & kAttrTimes) ? std::optional<std::uint32_t>(requested.modified) : std::nullopt);
  }
  Attributes attributes;
  int error = 0;
  if (!statPath(path, attributes, error)) return sendCardGone(id);
  if (!attributes.exists) return sendError(id, error, shown(path));
  if (attributes.folder) return sendError(id, EISDIR, shown(path));
  Handle* handle = newHandle();
  if (handle == nullptr) return sendStatus(id, kFailure, "too many files open at once; close one first");
  const std::string host = hostPath(path);
  int fd = -1;
  std::uint32_t generation = 0;
  if (!underLock(std::nullopt, &generation, [&] {
        fd = ::open(host.c_str(), O_RDONLY);
        if (fd < 0) error = errno;
      })) {
    return sendCardGone(id);
  }
  if (fd < 0) return sendError(id, error, shown(path));
  handle->used = true;
  handle->id = nextHandleId_++;
  if (nextHandleId_ == 0) nextHandleId_ = 1;
  handle->generation = generation;
  handle->path = path;
  handle->fd = fd;
  sendHandle(id, *handle);
}

void SftpServer::openUpload(std::uint32_t id, const Path& path, std::uint32_t flags,
                            std::optional<std::uint32_t> modified) {
  if (flags & kOpenAppend) {
    logWarning(kTag, "an append to %s refused: files on the card are only written whole", shown(path).c_str());
    return sendStatus(id, kUnsupported, "files on the card are only written whole, not appended to");
  }
  if (!takeGate(id)) return;
  // From here, every way out lets the gate go, unless the upload opened and keeps it.
  if (!allowed(id, path, true, "writing")) return giveGate();
  Path temporary = path;
  temporary.back() = std::string(fatName(temporary.back())) + kPartSuffix;
  if (!allowed(id, temporary, true, "writing")) return giveGate();
  if (isOpen(path) || isOpen(temporary)) {
    giveGate();
    return sendStatus(id, kFailure, "that file is already being written");
  }
  Attributes existing;
  int error = 0;
  if (!statPath(path, existing, error)) {
    giveGate();
    return sendCardGone(id);
  }
  if (existing.exists) {
    if (existing.folder) {
      giveGate();
      return sendError(id, EISDIR, shown(path));
    }
    if (flags & kOpenExclusive) {
      giveGate();
      return sendError(id, EEXIST, shown(path));
    }
    // With or without TRUNCATE (scp leaves it off, then sets the size it wrote), what the upload
    // writes replaces the file whole once it's closed.
  } else if (!(flags & kOpenCreate)) {
    giveGate();
    return sendError(id, ENOENT, shown(path));
  }
  const std::optional<FreeSpace> space = card_.freeSpace();
  if (space && space->freeBytes < keepFreeBytes_) {
    logWarning(kTag, "writing %s refused: %s free, and the ride log keeps %s", shown(path).c_str(),
               sizeText(space->freeBytes).c_str(), sizeText(keepFreeBytes_).c_str());
    giveGate();
    return sendStatus(id, kFailure, "the card is too full: the ride log keeps " + sizeText(keepFreeBytes_) + " free");
  }
  Handle* handle = newHandle();
  if (handle == nullptr) {
    giveGate();
    return sendStatus(id, kFailure, "too many files open at once; close one first");
  }
  const std::string host = hostPath(temporary);
  int fd = -1;
  std::uint32_t generation = 0;
  if (!underLock(std::nullopt, &generation, [&] {
        fd = ::open(host.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
        if (fd < 0) error = errno;
      })) {
    giveGate();
    return sendCardGone(id);
  }
  if (fd < 0) {
    giveGate();
    return sendError(id, error, shown(path));
  }
  handle->used = true;
  handle->id = nextHandleId_++;
  if (nextHandleId_ == 0) nextHandleId_ = 1;
  handle->generation = generation;
  handle->path = path;
  handle->fd = fd;
  handle->upload = true;
  handle->temporary = temporary;
  handle->modified = modified;
  logInfo(kTag, "receiving %s", shown(path).c_str());
  sendHandle(id, *handle);
}

void SftpServer::doWrite(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  const std::uint64_t offset = body.u64();
  const std::string_view data = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  if (!handle->upload) return sendStatus(id, kPermissionDenied, "that file was opened for reading");
  if (handle->failed) return sendStatus(id, kFailure, "an earlier write to that file failed");
  if (handle->fd < 0) return sendCardGone(id);
  if (offset > kMaxOffset || data.size() > kMaxOffset - offset) {
    handle->failed = true;
    return sendStatus(id, kFailure, "files on the card stop at 2 GB here");
  }
  const std::optional<FreeSpace> space = card_.freeSpace();
  if (space && space->freeBytes < keepFreeBytes_ + data.size()) {
    handle->failed = true;
    logWarning(kTag, "%s stopped: it would leave less than %s free for the ride log", shown(handle->path).c_str(),
               sizeText(keepFreeBytes_).c_str());
    return sendStatus(id, kFailure, "the card is too full: the ride log keeps " + sizeText(keepFreeBytes_) + " free");
  }
  // A piece at a time, each under the lock, so the ride log and the other consoles never wait on a
  // whole 32 KB.
  char* buffer = staging();
  int error = 0;
  std::size_t done = 0;
  while (done < data.size() && error == 0) {
    const std::size_t piece = buffer != nullptr ? std::min(data.size() - done, kStagingBytes) : data.size() - done;
    ssize_t put = 0;
    if (!underLock(handle->generation, nullptr, [&] {
          const char* from = data.data() + done;
          if (buffer != nullptr) {
            std::memcpy(buffer, from, piece);
            from = buffer;
          }
          put = ::pwrite(handle->fd, from, piece, static_cast<off_t>(offset + done));
          if (put <= 0) error = put < 0 ? errno : ENOSPC;
        })) {
      handle->fd = -1;
      handle->failed = true;
      return sendCardGone(id);
    }
    if (put > 0) done += static_cast<std::size_t>(put);
  }
  if (error != 0) {
    handle->failed = true;
    logWarning(kTag, "writing %s failed: %s", shown(handle->path).c_str(), cardErrorText(error).c_str());
    return sendError(id, error, shown(handle->path));
  }
  handle->bytes = std::max<std::uint64_t>(handle->bytes, offset + data.size());
  sendStatus(id, kOk, "");
}

void SftpServer::doRead(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  const std::uint64_t offset = body.u64();
  const std::uint32_t length = body.u32();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  if (handle->upload || handle->folder != nullptr) return sendStatus(id, kFailure, "that handle isn't a file being read");
  if (handle->fd < 0) return sendCardGone(id);
  if (offset >= kMaxOffset) return sendStatus(id, kEof, "");
  const auto wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>(std::min<std::uint64_t>(length, kMaxDataBytes), kMaxOffset - offset));
  const std::size_t start = beginReply(kData, id);
  const std::size_t lengthAt = output_.size();
  putU32(0);
  // A piece at a time, each under the lock, as WRITE does.
  char* buffer = staging();
  std::size_t got = 0;
  int error = 0;
  bool locked = true;
  bool ended = false;
  while (got < wanted && !ended && error == 0 && !overflowed_) {
    const std::size_t piece = buffer != nullptr ? std::min(wanted - got, kStagingBytes) : wanted - got;
    const std::size_t before = output_.size();
    char* into = output_.grow(piece);
    if (into == nullptr) {
      overflowed_ = true;
      break;
    }
    ssize_t read = 0;
    locked = underLock(handle->generation, nullptr, [&] {
      read = ::pread(handle->fd, buffer != nullptr ? buffer : into, piece, static_cast<off_t>(offset + got));
      if (read < 0) error = errno;
    });
    if (!locked) break;
    const std::size_t count = read > 0 ? static_cast<std::size_t>(read) : 0;
    if (buffer != nullptr && count > 0) std::memcpy(into, buffer, count);
    output_.shrinkTo(before + count);
    got += count;
    ended = count < piece;  // the end of the file
  }
  if (!locked) {
    output_.shrinkTo(start);
    handle->fd = -1;
    return sendCardGone(id);
  }
  if (error != 0) {
    output_.shrinkTo(start);
    return sendError(id, error, shown(handle->path));
  }
  if (got == 0) {
    output_.shrinkTo(start);
    return sendStatus(id, kEof, "");
  }
  writeU32(output_.at(lengthAt), static_cast<std::uint32_t>(got));
  endReply(start);
  handle->bytes += got;
}

void SftpServer::doClose(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  if (handle->upload) return closeUpload(id, *handle);
  int error = 0;
  underLock(handle->generation, nullptr, [&] {
    if (handle->folder != nullptr) {
      ::closedir(static_cast<DIR*>(handle->folder));
    } else if (handle->fd >= 0 && ::close(handle->fd) != 0) {
      error = errno;
    }
  });
  if (handle->folder == nullptr && handle->bytes > 0) {
    logInfo(kTag, "sent %s (%s)", shown(handle->path).c_str(), sizeText(handle->bytes).c_str());
  }
  const Path path = handle->path;
  *handle = Handle{};
  if (error != 0) return sendError(id, error, shown(path));
  sendStatus(id, kOk, "");
}

void SftpServer::closeUpload(std::uint32_t id, Handle& upload) {
  int error = 0;
  const std::uint32_t generation = upload.generation;
  const bool locked = underLock(generation, nullptr, [&] {
    if (upload.fd >= 0 && ::close(upload.fd) != 0) error = errno;
  });
  const Path path = upload.path;
  const Path temporary = upload.temporary;
  const bool failed = upload.failed || upload.fd < 0;
  const std::uint64_t bytes = upload.bytes;
  const std::optional<std::uint32_t> modified = upload.modified;
  upload = Handle{};
  const std::string host = hostPath(path);
  const std::string temporaryHost = hostPath(temporary);
  if (!locked) {
    giveGate();
    logWarning(kTag, "%s not written: the card went away", shown(path).c_str());
    return sendCardGone(id);
  }
  if (failed || error != 0) {
    underLock(generation, nullptr, [&] { ::unlink(temporaryHost.c_str()); });
    giveGate();
    logWarning(kTag, "%s not written%s%s", shown(path).c_str(), error != 0 ? ": " : "",
               error != 0 ? cardErrorText(error).c_str() : "");
    if (error != 0) return sendError(id, error, shown(path));
    return sendStatus(id, kFailure, "a write failed, so nothing was written");
  }
  // The rules again: the ride log may have started meanwhile.
  if (!allowed(id, path, true, "writing")) {
    underLock(generation, nullptr, [&] { ::unlink(temporaryHost.c_str()); });
    return giveGate();
  }
  Attributes existing;
  int statError = 0;
  if (!statPath(path, existing, statError)) {
    giveGate();
    return sendCardGone(id);
  }
  int replaceError = 0;
  bool removedOld = false;
  bool replaced = false;
  const bool mounted = underLock(generation, nullptr, [&] {
    if (existing.exists) {
      if (existing.folder) {
        replaceError = EISDIR;
        ::unlink(temporaryHost.c_str());
        return;
      }
      if (::unlink(host.c_str()) != 0) {
        replaceError = errno;
        ::unlink(temporaryHost.c_str());
        return;
      }
      removedOld = true;
    }
    if (::rename(temporaryHost.c_str(), host.c_str()) != 0) {
      replaceError = errno;
      return;
    }
    replaced = true;
    if (modified) {
      utimbuf times = {};
      times.actime = static_cast<std::time_t>(*modified);
      times.modtime = static_cast<std::time_t>(*modified);
      ::utime(host.c_str(), &times);
    }
  });
  giveGate();
  if (!mounted) return sendCardGone(id);
  if (!replaced) {
    if (removedOld) {
      logError(kTag, "%s was removed to make way, but the new one couldn't take its name (%s); it's in %s",
               shown(path).c_str(), cardErrorText(replaceError).c_str(), shown(temporary).c_str());
    } else {
      logWarning(kTag, "%s not written: %s", shown(path).c_str(), cardErrorText(replaceError).c_str());
    }
    return sendError(id, replaceError, shown(path));
  }
  changed(path);
  logInfo(kTag, "wrote %s (%s)%s", shown(path).c_str(), sizeText(bytes).c_str(),
          existing.exists ? ", replacing what was there" : "");
  sendStatus(id, kOk, "");
}

void SftpServer::doOpenFolder(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  Attributes attributes;
  int error = 0;
  if (!statPath(path, attributes, error)) return sendCardGone(id);
  if (!attributes.exists) return sendError(id, error, shown(path));
  if (!attributes.folder) return sendError(id, ENOTDIR, shown(path));
  Handle* handle = newHandle();
  if (handle == nullptr) return sendStatus(id, kFailure, "too many files open at once; close one first");
  const std::string host = hostPath(path);
  DIR* folder = nullptr;
  std::uint32_t generation = 0;
  if (!underLock(std::nullopt, &generation, [&] {
        folder = ::opendir(host.c_str());
        if (folder == nullptr) error = errno;
      })) {
    return sendCardGone(id);
  }
  if (folder == nullptr) return sendError(id, error, shown(path));
  handle->used = true;
  handle->id = nextHandleId_++;
  if (nextHandleId_ == 0) nextHandleId_ = 1;
  handle->generation = generation;
  handle->path = path;
  handle->folder = folder;
  sendHandle(id, *handle);
}

void SftpServer::doReadFolder(std::uint32_t id, Reader& body) {
  const std::string_view token = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Handle* handle = findHandle(token);
  if (handle == nullptr) return sendNoHandle(id);
  if (handle->folder == nullptr) return sendStatus(id, kFailure, "that handle isn't a folder");
  const std::size_t start = beginReply(kName, id);
  const std::size_t countAt = output_.size();
  putU32(0);
  std::uint32_t count = 0;
  const std::string base = hostPath(handle->path);
  const bool locked = underLock(handle->generation, nullptr, [&] {
    DIR* folder = static_cast<DIR*>(handle->folder);
    while (count < kNamesPerReply) {
      const dirent* entry = ::readdir(folder);
      if (entry == nullptr) return;
      const std::string_view name = entry->d_name;
      if (name == "." || name == "..") continue;
      Attributes attributes;
      attributes.exists = true;
      attributes.folder = entry->d_type == DT_DIR;
      struct stat status = {};
      const std::string child = base + "/" + std::string(name);
      if (::stat(child.c_str(), &status) == 0) {
        attributes.folder = S_ISDIR(status.st_mode);
        attributes.bytes = attributes.folder ? 0 : static_cast<std::uint64_t>(status.st_size);
        attributes.modified = static_cast<std::uint32_t>(status.st_mtime);
      }
      putString(name);
      putString(longName(name, attributes));
      putAttributes(attributes);
      count++;
    }
  });
  if (!locked) {
    output_.shrinkTo(start);
    handle->folder = nullptr;  // went with the card
    return sendCardGone(id);
  }
  if (count == 0) {
    output_.shrinkTo(start);
    return sendStatus(id, kEof, "");
  }
  writeU32(output_.at(countAt), count);
  endReply(start);
}

void SftpServer::doRemove(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  if (!takeGate(id)) return;
  if (!allowed(id, path, false, "removing")) return giveGate();
  if (isOpen(path)) {
    giveGate();
    return sendStatus(id, kFailure, "that file is being written");
  }
  Attributes attributes;
  int error = 0;
  if (!statPath(path, attributes, error)) {
    giveGate();
    return sendCardGone(id);
  }
  if (!attributes.exists || attributes.folder) {
    giveGate();
    return sendError(id, attributes.exists ? EISDIR : error, shown(path));
  }
  const std::string host = hostPath(path);
  const bool locked = underLock(std::nullopt, nullptr, [&] {
    if (::unlink(host.c_str()) != 0) error = errno;
  });
  giveGate();
  if (!locked) return sendCardGone(id);
  if (error != 0) return sendError(id, error, shown(path));
  changed(path);
  logInfo(kTag, "removed %s", shown(path).c_str());
  sendStatus(id, kOk, "");
}

void SftpServer::doMakeFolder(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  body.attributes();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  if (!takeGate(id)) return;
  if (!allowed(id, path, true, "making the folder")) return giveGate();
  const std::string host = hostPath(path);
  int error = 0;
  const bool locked = underLock(std::nullopt, nullptr, [&] {
    if (::mkdir(host.c_str(), 0775) != 0) error = errno;
  });
  giveGate();
  if (!locked) return sendCardGone(id);
  if (error != 0) return sendError(id, error, shown(path));
  changed(path);
  logInfo(kTag, "made the folder %s", shown(path).c_str());
  sendStatus(id, kOk, "");
}

void SftpServer::doRemoveFolder(std::uint32_t id, Reader& body) {
  const std::string_view text = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path path;
  if (!parsePath(text, path)) return sendStatus(id, kNoSuchFile, "not a path on the card");
  if (!takeGate(id)) return;
  if (!allowed(id, path, false, "removing the folder")) return giveGate();
  if (isOpen(path)) {
    giveGate();
    return sendStatus(id, kFailure, "a file in it is being written");
  }
  const std::string host = hostPath(path);
  int error = 0;
  const bool locked = underLock(std::nullopt, nullptr, [&] {
    if (::rmdir(host.c_str()) != 0) error = errno;
  });
  giveGate();
  if (!locked) return sendCardGone(id);
  if (error != 0) return sendError(id, error, shown(path));
  changed(path);
  logInfo(kTag, "removed the folder %s", shown(path).c_str());
  sendStatus(id, kOk, "");
}

void SftpServer::doRename(std::uint32_t id, Reader& body) {
  const std::string_view fromText = body.string();
  const std::string_view toText = body.string();
  if (!body.ok()) return sendBadMessage(id);
  Path from;
  Path to;
  if (!parsePath(fromText, from) || !parsePath(toText, to)) {
    return sendStatus(id, kNoSuchFile, "not a path on the card");
  }
  if (!takeGate(id)) return;
  if (!allowed(id, from, false, "moving") || !allowed(id, to, true, "moving")) return giveGate();
  if (to.size() > from.size() && within(to, from)) {
    giveGate();
    return sendStatus(id, kFailure, "a folder can't go inside itself");
  }
  if (isOpen(from) || isOpen(to)) {
    giveGate();
    return sendStatus(id, kFailure, "that file is being written");
  }
  Attributes source;
  int error = 0;
  if (!statPath(from, source, error)) {
    giveGate();
    return sendCardGone(id);
  }
  if (!source.exists) {
    giveGate();
    return sendError(id, error, shown(from));
  }
  // Version 3's rename never replaces. A change of case is the same entry, so it may.
  if (!samePath(from, to)) {
    Attributes target;
    if (!statPath(to, target, error)) {
      giveGate();
      return sendCardGone(id);
    }
    if (target.exists) {
      giveGate();
      return sendError(id, EEXIST, shown(to));
    }
  }
  const std::string fromHost = hostPath(from);
  const std::string toHost = hostPath(to);
  error = 0;
  const bool locked = underLock(std::nullopt, nullptr, [&] {
    if (::rename(fromHost.c_str(), toHost.c_str()) != 0) error = errno;
  });
  giveGate();
  if (!locked) return sendCardGone(id);
  if (error != 0) return sendError(id, error, shown(from));
  changed(from);
  changed(to);
  logInfo(kTag, "moved %s to %s", shown(from).c_str(), shown(to).c_str());
  sendStatus(id, kOk, "");
}

// ----------------------------------------------------------------------------------- helpers

bool SftpServer::parsePath(std::string_view text, Path& path) {
  path.clear();
  for (const char c : text) {
    // \ separates folders for FatFs too, and it ends a path at a control character; : names a
    // drive. Any of them would let a path mean something other than what's checked here.
    if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < ' ') return false;
  }
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t slash = std::min(text.find('/', at), text.size());
    const std::string_view part = text.substr(at, slash - at);
    if (part.empty() || part == ".") {
      // nothing
    } else if (part == "..") {
      if (!path.empty()) path.pop_back();
    } else {
      if (fatName(part).empty()) return false;
      path.emplace_back(part);
    }
    at = slash + 1;
  }
  return true;
}

std::string SftpServer::hostPath(const Path& path) const {
  std::string full = card_.mountPoint();
  for (const std::string& part : path) full += "/" + part;
  return full;
}

std::string SftpServer::shown(const Path& path) {
  if (path.empty()) return "/";
  std::string text;
  for (const std::string& part : path) text += "/" + part;
  return text;
}

SftpServer::Handle* SftpServer::findHandle(std::string_view token) {
  if (token.size() != 4) return nullptr;
  const std::uint32_t id = readU32(token.data());
  for (Handle& handle : handles_) {
    if (handle.used && handle.id == id) return &handle;
  }
  return nullptr;
}

SftpServer::Handle* SftpServer::newHandle() {
  for (Handle& handle : handles_) {
    if (!handle.used) {
      handle = Handle{};
      return &handle;
    }
  }
  return nullptr;
}

bool SftpServer::isOpen(const Path& path) const {
  for (const Handle& handle : handles_) {
    if (handle.used && handle.upload && (within(handle.path, path) || within(handle.temporary, path))) return true;
  }
  return false;
}

void SftpServer::release(Handle& handle) {
  const std::string temporaryHost = handle.upload ? hostPath(handle.temporary) : std::string();
  underLock(handle.generation, nullptr, [&] {
    if (handle.folder != nullptr) ::closedir(static_cast<DIR*>(handle.folder));
    if (handle.fd >= 0) ::close(handle.fd);
    if (handle.upload) ::unlink(temporaryHost.c_str());
  });
  if (handle.upload) logWarning(kTag, "%s wasn't finished, so nothing was written", shown(handle.path).c_str());
  handle = Handle{};
}

char* SftpServer::staging() {
  if (!stagingTried_) {
    stagingTried_ = true;
    staging_.reset(new (std::nothrow) char[kStagingBytes]);
  }
  return staging_.get();
}

template <typename Operation>
bool SftpServer::underLock(const std::optional<std::uint32_t>& generation, std::uint32_t* now, Operation&& operation) {
  const std::lock_guard<std::mutex> lock(card_.lock());
  const CardState state = card_.state();
  if (!state.mounted || (generation && state.generation != *generation)) return false;
  if (now != nullptr) *now = state.generation;
  operation();
  return true;
}

bool SftpServer::statPath(const Path& path, Attributes& attributes, int& error) {
  attributes = {};
  error = 0;
  const std::string host = hostPath(path);
  return underLock(std::nullopt, nullptr, [&] {
    if (path.empty()) {
      attributes.exists = true;
      attributes.folder = true;
      return;
    }
    struct stat status = {};
    if (::stat(host.c_str(), &status) != 0) {
      error = errno;
      return;
    }
    attributes.exists = true;
    attributes.folder = S_ISDIR(status.st_mode);
    attributes.bytes = attributes.folder ? 0 : static_cast<std::uint64_t>(status.st_size);
    attributes.modified = static_cast<std::uint32_t>(status.st_mtime);
  });
}

bool SftpServer::allowed(std::uint32_t id, const Path& path, bool creating, const char* what) {
  const std::optional<std::string> refused = cardChangeRefusal(path, creating, card_.facts());
  if (!refused) return true;
  logWarning(kTag, "%s %s refused: %s", what, shown(path).c_str(), refused->c_str());
  sendStatus(id, kPermissionDenied, *refused);
  return false;
}

bool SftpServer::takeGate(std::uint32_t id) {
  std::string holder;
  if (!gate_.take(console_, holder)) {
    logWarning(kTag, "the %s is changing the card, so a change over SFTP was refused", holder.c_str());
    sendStatus(id, kPermissionDenied, "the " + holder + " is changing the card; try again when it has finished");
    return false;
  }
  holdsGate_ = true;
  return true;
}

void SftpServer::giveGate() {
  if (!holdsGate_) return;
  for (const Handle& handle : handles_) {
    if (handle.used && handle.upload) return;
  }
  gate_.give(console_);
  holdsGate_ = false;
}

void SftpServer::changed(const Path& path) { card_.changed(!path.empty() && sameName(path[0], kRides)); }

// ----------------------------------------------------------------------------------- replies

std::size_t SftpServer::beginReply(std::uint8_t type, std::optional<std::uint32_t> id) {
  const std::size_t start = output_.size();
  putU32(0);  // the length, once it's known
  put(&type, 1);
  if (id) putU32(*id);
  return start;
}

void SftpServer::endReply(std::size_t start) {
  if (overflowed_) return;
  writeU32(output_.at(start), static_cast<std::uint32_t>(output_.size() - start - 4));
}

void SftpServer::put(const void* bytes, std::size_t count) {
  char* into = output_.grow(count);
  if (into == nullptr) {
    overflowed_ = true;
    return;
  }
  std::memcpy(into, bytes, count);
}

void SftpServer::putU32(std::uint32_t value) {
  char bytes[4];
  writeU32(bytes, value);
  put(bytes, sizeof bytes);
}

void SftpServer::putU64(std::uint64_t value) {
  putU32(static_cast<std::uint32_t>(value >> 32));
  putU32(static_cast<std::uint32_t>(value));
}

void SftpServer::putString(std::string_view text) {
  putU32(static_cast<std::uint32_t>(text.size()));
  put(text.data(), text.size());
}

void SftpServer::putAttributes(const Attributes& attributes) {
  putU32(kAttrSize | kAttrPermissions | kAttrTimes);
  putU64(attributes.bytes);
  putU32(attributes.folder ? kFolderMode : kFileMode);
  putU32(attributes.modified);  // accessed: FAT keeps only a date for it
  putU32(attributes.modified);
}

void SftpServer::sendStatus(std::uint32_t id, std::uint32_t code, std::string_view message) {
  const std::size_t start = beginReply(kStatus, id);
  putU32(code);
  putString(message);
  putString("en");
  endReply(start);
}

void SftpServer::sendError(std::uint32_t id, int error, const std::string& what) {
  sendStatus(id, statusFor(error), what + ": " + cardErrorText(error));
}

void SftpServer::sendBadMessage(std::uint32_t id) { sendStatus(id, kBadMessage, "a request this server couldn't read"); }

void SftpServer::sendNoHandle(std::uint32_t id) { sendStatus(id, kFailure, "no such handle: it was closed, or never opened"); }

void SftpServer::sendCardGone(std::uint32_t id) {
  sendStatus(id, kFailure, "no card: the microSD card isn't mounted, or went away");
}

void SftpServer::sendHandle(std::uint32_t id, const Handle& handle) {
  char token[4];
  writeU32(token, handle.id);
  const std::size_t start = beginReply(kHandle, id);
  putString(std::string_view(token, sizeof token));
  endReply(start);
}

std::string SftpServer::longName(std::string_view name, const Attributes& attributes) {
  // As `ls -l` prints it, which is how sftp's `ls -l` shows it.
  char date[32] = "";
  const auto seconds = static_cast<std::time_t>(attributes.modified);
  std::tm parts = {};
  if (gmtime_r(&seconds, &parts) != nullptr) std::strftime(date, sizeof date, "%b %e %H:%M", &parts);
  char head[96];
  std::snprintf(head, sizeof head, "%s    1 0        0        %12llu %s ", attributes.folder ? "drwxr-xr-x" : "-rw-r--r--",
                static_cast<unsigned long long>(attributes.bytes), date);
  return std::string(head) + std::string(name);
}

}  // namespace emcool::files
