#include "emcool/ride_log/log_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "card_layout.hpp"
#include "emcool/log.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/signals/registry.hpp"
#include "emcool/signals/ride_log_policy.hpp"

namespace emcool::ride_log {

using text::TextBuffer;

namespace {

constexpr const char* kTag = "ride-log";
// The metadata records that open a part (~25 KB for the signal registry) plus a full segment.
constexpr std::uint32_t kMinPartBytes = 64 * 1024;
// Kept free beyond a whole part, for the directory and FAT growth that come with it.
constexpr std::uint64_t kReserveBytes = 1024 * 1024;

const char* sourceName(signals::SignalSource source) {
  switch (source) {
    case signals::SignalSource::kStream:
      return "stream";
    case signals::SignalSource::kPoll:
      return "poll";
    case signals::SignalSource::kSensor:
      return "sensor";
  }
  return "stream";
}

// The numbers of `directory`'s entries named <prefix><digits><suffix>, ascending; none if it
// can't be listed (layout::numberedEntries has warned).
std::vector<std::uint32_t> numberedEntries(const std::string& directory, std::string_view prefix,
                                           std::string_view suffix) {
  return layout::numberedEntries(directory, prefix, suffix).value_or(std::vector<std::uint32_t>{});
}

bool writeFully(int fd, const std::uint8_t* data, std::size_t length, std::uint64_t offset) {
  while (length > 0) {
    const ssize_t written = pwrite(fd, data, length, static_cast<off_t>(offset));
    if (written <= 0) {
      return false;
    }
    data += written;
    length -= static_cast<std::size_t>(written);
    offset += static_cast<std::uint64_t>(written);
  }
  return true;
}

}  // namespace

LogWriter::LogWriter(LogWriterConfig config, const SessionId& session, std::span<std::uint8_t> scratch,
                     CardSpace& space)
    : config_(std::move(config)), session_(session), scratch_(scratch), space_(space) {}

LogWriter::~LogWriter() { close(); }

bool LogWriter::open() {
  close();
  if (scratch_.size() < kRecordCapacityBytes || config_.partBytes % kSectorBytes != 0 ||
      config_.partBytes < kMinPartBytes || config_.fillStepBytes == 0 || config_.fillStepBytes % kSectorBytes != 0) {
    logError(kTag, "writer misconfigured (scratch %zu, part %u, step %u)", scratch_.size(),
             static_cast<unsigned>(config_.partBytes), static_cast<unsigned>(config_.fillStepBytes));
    return false;
  }
  if (mkdir(config_.rootDirectory.c_str(), 0775) != 0 && errno != EEXIST) {
    return fail("mkdir", config_.rootDirectory);
  }
  // Numbered before any old boot is rotated away, so numbers only ever go up.
  const std::vector<std::uint32_t> boots = numberedEntries(config_.rootDirectory, "", "");
  bootNumber_ = (boots.empty() ? 0 : boots.back()) + 1;
  std::uint32_t partsOnCard = 0;
  for (const std::uint32_t boot : boots) {
    partsOnCard += static_cast<std::uint32_t>(numberedEntries(bootDirectory(boot), layout::kPartPrefix, layout::kPartSuffix).size());
  }
  counters_.partsOnCard.store(partsOnCard, std::memory_order_relaxed);
  nextPartBlocked_ = false;
  if (!makeRoomForPart()) {
    return false;
  }
  // EEXIST can only mean a directory appeared since the listing; take the next number.
  while (mkdir(bootDirectory().c_str(), 0775) != 0) {
    if (errno != EEXIST) {
      return fail("mkdir", bootDirectory());
    }
    bootNumber_++;
  }
  counters_.bootNumber.store(bootNumber_, std::memory_order_relaxed);
  if (!createPart(current_, 1)) {
    return false;
  }
  publishExtent();
  logInfo(kTag, "logging to %s (session %s)", bootDirectory().c_str(), session_.hex.data());
  return true;
}

void LogWriter::close() {
  for (Part* part : {&current_, &next_}) {
    if (part->fd >= 0) {
      if (::close(part->fd) != 0) {
        logWarning(kTag, "closing %s failed (errno %d)", partPath(part->number).c_str(), errno);
      }
    }
    *part = Part{};
  }
  nextReady_ = false;
  publishExtent();
}

bool LogWriter::write(SegmentBuffer& segment) {
  if (!isOpen()) {
    return false;
  }
  const std::size_t length = paddedRecordLength(segment.payloadLength);
  if (current_.writeOffset + length > config_.partBytes && !switchToNextPart()) {
    return false;
  }
  if (!current_.metadataWritten && !writeMetadataRecords(segment.trust)) {
    return false;
  }
  finalizeRecord(segment.bytes, session_, nextRecordIndex_, segment.payloadLength);
  return writeRecordAtEnd(segment.bytes, length);
}

bool LogWriter::prepareAhead() {
  if (!isOpen()) {
    return false;
  }
  if (current_.allocatedBytes < config_.partBytes) {
    zeroFillStep(current_);
    return true;
  }
  if (next_.fd < 0) {
    if (nextPartBlocked_) {
      return false;
    }
    // No room even after rotating: stop preparing and let the current part fill. Switching
    // tries once more, and only then does logging stop.
    if (!createPart(next_, current_.number + 1) && isOpen()) {
      nextPartBlocked_ = true;
      return false;
    }
    return isOpen();
  }
  if (!nextReady_) {
    zeroFillStep(next_);
    return true;
  }
  return false;
}

// False either because there is no room even after rotating (the writer stays open) or
// because the file could not be created (the writer has closed itself).
bool LogWriter::createPart(Part& part, std::uint32_t number) {
  if (!makeRoomForPart()) {
    return false;
  }
  const std::string path = partPath(number);
  // O_EXCL: a part is never reopened, so an existing file means something is wrong — never
  // write over it.
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0664);
  if (fd < 0) {
    return fail("create", path);
  }
  part = Part{};
  part.fd = fd;
  part.number = number;
  counters_.partsOpened.fetch_add(1, std::memory_order_relaxed);
  counters_.partsOnCard.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Extends a part with zeros. FAT allocates the clusters here, ahead of time, instead of
// while a record is being written; the fsync at the end commits the part's final size.
bool LogWriter::zeroFillStep(Part& part) {
  const std::uint64_t target =
      std::min<std::uint64_t>(part.allocatedBytes + config_.fillStepBytes, config_.partBytes);
  std::memset(scratch_.data(), 0, kRecordCapacityBytes);
  while (part.allocatedBytes < target) {
    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(kRecordCapacityBytes, target - part.allocatedBytes));
    if (!writeMakingRoom(part.fd, scratch_.data(), chunk, part.allocatedBytes)) {
      return fail("zero-fill", partPath(part.number));
    }
    part.allocatedBytes += chunk;
  }
  if (part.allocatedBytes == config_.partBytes) {
    if (fsync(part.fd) != 0) {
      return fail("fsync", partPath(part.number));
    }
    if (&part == &next_) {
      nextReady_ = true;
    }
  }
  return true;
}

bool LogWriter::switchToNextPart() {
  if (next_.fd < 0 && !createPart(next_, current_.number + 1)) {
    if (isOpen()) {
      errno = ENOSPC;
      return fail("make room for", partPath(current_.number + 1));
    }
    return false;
  }
  if (!nextReady_) {
    counters_.partsPreparedInLine.fetch_add(1, std::memory_order_relaxed);
    logWarning(kTag, "part %u filled before part %u was prepared; preparing it now", static_cast<unsigned>(current_.number),
               static_cast<unsigned>(next_.number));
    while (!nextReady_) {
      if (!zeroFillStep(next_)) {
        return false;
      }
    }
  }
  if (::close(current_.fd) != 0) {
    return fail("close", partPath(current_.number));
  }
  current_ = next_;
  next_ = Part{};
  nextReady_ = false;
  counters_.currentPart.store(current_.number, std::memory_order_relaxed);
  publishExtent();
  return true;
}

// Deletes the oldest parts until a whole new part, plus a reserve, fits.
bool LogWriter::makeRoomForPart() {
  const std::uint64_t needed = std::uint64_t{config_.partBytes} + kReserveBytes;
  while (true) {
    const std::optional<std::uint64_t> free = space_.freeBytes();
    if (!free) {
      logWarning(kTag, "cannot read the card's free space; going ahead and relying on ENOSPC");
      return true;
    }
    if (*free >= needed) {
      return true;
    }
    if (!deleteOldestPart()) {
      logError(kTag, "the card is full (%llu KB free, %llu KB needed) and none of the ride log is left to delete",
               static_cast<unsigned long long>(*free / 1024), static_cast<unsigned long long>(needed / 1024));
      return false;
    }
  }
}

// First in, first out: the lowest boot, then its lowest part. Never this boot's current or
// prepared part, and never anything not named like a part. A boot directory goes with its
// last part, unless something else is in it.
bool LogWriter::deleteOldestPart() {
  for (const std::uint32_t boot : numberedEntries(config_.rootDirectory, "", "")) {
    const std::string directory = bootDirectory(boot);
    const std::vector<std::uint32_t> parts = numberedEntries(directory, layout::kPartPrefix, layout::kPartSuffix);
    for (const std::uint32_t part : parts) {
      if (boot == bootNumber_ && (part == current_.number || part == next_.number)) {
        continue;
      }
      const std::string path = partPath(boot, part);
      if (unlink(path.c_str()) != 0) {
        logWarning(kTag, "could not delete %s to make room (errno %d); trying the next oldest", path.c_str(), errno);
        continue;
      }
      counters_.partsRotated.fetch_add(1, std::memory_order_relaxed);
      counters_.partsOnCard.fetch_sub(1, std::memory_order_relaxed);
      logInfo(kTag, "card full: deleted %s, the oldest part, to make room", path.c_str());
      if (boot != bootNumber_ && parts.size() == 1 && rmdir(directory.c_str()) != 0) {
        logInfo(kTag, "kept %s: it holds files that aren't ride-log parts (errno %d)", directory.c_str(), errno);
      }
      return true;
    }
  }
  return false;
}

// A write that runs out of room rotates the oldest part away and tries again, for when the
// free-space figure was optimistic.
bool LogWriter::writeMakingRoom(int fd, const std::uint8_t* data, std::size_t length, std::uint64_t offset) {
  while (!writeFully(fd, data, length, offset)) {
    const int writeError = errno;
    if (writeError != ENOSPC) {
      errno = writeError;
      return false;
    }
    if (!deleteOldestPart()) {
      errno = ENOSPC;
      return false;
    }
  }
  return true;
}

// Every logged signal's unit, group and source, as the v3 header's `signals` map,
// split across as many records as it takes. The laptop reads them before the rows.
bool LogWriter::writeMetadataRecords(clock::ClockTrust trust) {
  const std::span<const signals::SignalDef> registry = signals::signalRegistry();
  std::size_t next = 0;
  while (true) {
    while (next < registry.size() && signals::isExcludedFromRideLog(registry[next].key)) {
      next++;
    }
    if (next >= registry.size()) {
      break;
    }
    char* payload = reinterpret_cast<char*>(scratch_.data() + kRecordHeaderBytes);
    TextBuffer header(payload, kMaxPayloadBytes);
    header.append("{\"v\":3,\"session\":");
    header.appendJsonString(session_.hex.data());
    header.append(",\"trust\":");
    header.appendJsonString(clock::clockTrustName(trust));
    header.append(",\"signals\":{");
    // Room for the closing "}}\n" is held back while entries are added.
    constexpr std::size_t kClosingBytes = 3;
    TextBuffer entries(payload, kMaxPayloadBytes - kClosingBytes, header.length());
    std::size_t added = 0;
    while (next < registry.size()) {
      const signals::SignalDef& signal = registry[next];
      if (signals::isExcludedFromRideLog(signal.key)) {
        next++;
        continue;
      }
      const std::size_t before = entries.length();
      if (added > 0) {
        entries.append(",");
      }
      entries.appendJsonString(signal.key);
      entries.append(":[");
      entries.appendJsonString(signal.unit);
      entries.append(",");
      entries.appendJsonString(signal.group);
      entries.append(",");
      entries.appendJsonString(sourceName(signal.source));
      entries.append("]");
      if (entries.overflowed()) {
        entries.truncate(before);
        break;
      }
      added++;
      next++;
    }
    if (added == 0) {
      logError(kTag, "the metadata for %s does not fit in one record", registry[next].key);
      return false;
    }
    TextBuffer closing(payload, kMaxPayloadBytes, entries.length());
    closing.append("}}\n");
    const std::size_t length = finalizeRecord(scratch_.data(), session_, nextRecordIndex_, closing.length());
    if (!writeRecordAtEnd(scratch_.data(), length)) {
      return false;
    }
  }
  current_.metadataWritten = true;
  return true;
}

bool LogWriter::writeRecordAtEnd(const std::uint8_t* record, std::size_t length) {
  if (!writeMakingRoom(current_.fd, record, length, current_.writeOffset)) {
    return fail("write", partPath(current_.number));
  }
  if (fsync(current_.fd) != 0) {
    return fail("fsync", partPath(current_.number));
  }
  current_.writeOffset += length;
  current_.allocatedBytes = std::max(current_.allocatedBytes, current_.writeOffset);
  publishExtent();
  nextRecordIndex_++;
  counters_.recordsWritten.fetch_add(1, std::memory_order_relaxed);
  counters_.bytesWritten.fetch_add(length, std::memory_order_relaxed);
  return true;
}

bool LogWriter::fail(const char* operation, const std::string& path) {
  logError(kTag, "%s %s failed (errno %d: %s); the ride log stops until it is reopened", operation, path.c_str(),
           errno, std::strerror(errno));
  counters_.writeErrors.fetch_add(1, std::memory_order_relaxed);
  close();
  return false;
}

std::string LogWriter::bootDirectory() const { return bootDirectory(bootNumber_); }

std::string LogWriter::bootDirectory(std::uint32_t bootNumber) const {
  return layout::bootDirectory(config_.rootDirectory, bootNumber);
}

std::string LogWriter::partPath(std::uint32_t number) const { return partPath(bootNumber_, number); }

std::string LogWriter::partPath(std::uint32_t bootNumber, std::uint32_t number) const {
  return layout::partPath(config_.rootDirectory, bootNumber, number);
}

void LogWriter::publishExtent() {
  const std::lock_guard<std::mutex> lock(extentMutex_);
  if (isOpen()) {
    extent_ = LiveExtent{bootNumber_, current_.number, current_.writeOffset};
  } else {
    extent_.reset();
  }
}

std::optional<LiveExtent> LogWriter::liveExtent() const {
  const std::lock_guard<std::mutex> lock(extentMutex_);
  return extent_;
}

}  // namespace emcool::ride_log
