#include "emcool/ride_log/download.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "card_layout.hpp"
#include "emcool/log.hpp"
#include "emcool/ride_log/record.hpp"

namespace emcool::ride_log {

namespace {

constexpr const char* kTag = "ride-log";

std::string describeErrno(const char* operation, const std::string& path) {
  return std::string(operation) + " " + path + " failed (errno " + std::to_string(errno) + ": " +
         std::strerror(errno) + ")";
}

}  // namespace

std::optional<std::uint64_t> writtenLength(int fd, std::string& problem) {
  struct stat info = {};
  if (fstat(fd, &info) != 0) {
    problem = "fstat failed (errno " + std::to_string(errno) + ")";
    return std::nullopt;
  }
  const std::uint64_t fileBytes = static_cast<std::uint64_t>(info.st_size);
  const std::uint64_t sectors = (fileBytes + kSectorBytes - 1) / kSectorBytes;
  std::array<std::uint8_t, kSectorBytes> sector = {};
  bool failed = false;
  // Whether sector `index` is all zeros (a short last sector counts by what it holds).
  const auto isZero = [&](std::uint64_t index) {
    if (failed) return true;
    const ssize_t got = pread(fd, sector.data(), sector.size(), static_cast<off_t>(index * kSectorBytes));
    if (got < 0) {
      problem = "read failed (errno " + std::to_string(errno) + ")";
      failed = true;
      return true;
    }
    return std::all_of(sector.begin(), sector.begin() + got, [](std::uint8_t byte) { return byte == 0; });
  };

  std::uint64_t end = 0;
  while (true) {
    // The first zero sector at or after `end`, if the sectors from `end` on are records then zeros.
    std::uint64_t high = sectors;
    while (end < high) {
      const std::uint64_t middle = end + (high - end) / 2;
      if (isZero(middle)) {
        high = middle;
      } else {
        end = middle + 1;
      }
    }
    // Anything past it? Probing at doubling distances finds records after a zeroed run.
    std::optional<std::uint64_t> beyond;
    for (std::uint64_t step = 1; end + step < sectors; step *= 2) {
      if (!isZero(end + step)) {
        beyond = end + step;
        break;
      }
    }
    if (failed) return std::nullopt;
    if (!beyond) break;
    end = *beyond + 1;
  }
  return std::min(end * kSectorBytes, fileBytes);
}

std::optional<CardListing> listCard(DownloadCard& card, std::string& problem) {
  const std::lock_guard<std::mutex> lock(card.mountLock());
  const DownloadCard::State state = card.state();
  if (!state.mounted) {
    problem = "there is no card in the bike, or it isn't mounted";
    return std::nullopt;
  }
  const std::optional<std::vector<std::uint32_t>> boots = layout::numberedEntries(card.rootDirectory(), "", "");
  if (!boots) {
    problem = "the card's " + card.rootDirectory() + " can't be listed";
    return std::nullopt;
  }
  CardListing listing;
  for (auto boot = boots->rbegin(); boot != boots->rend(); ++boot) {
    std::optional<std::vector<std::uint32_t>> parts = layout::numberedEntries(
        layout::bootDirectory(card.rootDirectory(), *boot), layout::kPartPrefix, layout::kPartSuffix);
    if (!parts) continue;
    CardListing::Boot entry{*boot, {}, 0};
    for (const std::uint32_t part : *parts) {
      const bool live = state.live && state.live->boot == *boot && state.live->part == part;
      if (state.live && state.live->boot == *boot && part > state.live->part) continue;  // being prepared
      std::uint64_t bytes = 0;
      if (live) {
        bytes = state.live->durableBytes;
      } else {
        const std::string path = layout::partPath(card.rootDirectory(), *boot, part);
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;  // deleted to make room since the listing
        std::string unreadable;
        const std::optional<std::uint64_t> length = writtenLength(fd, unreadable);
        ::close(fd);
        if (!length) {
          problem = path + ": " + unreadable;
          return std::nullopt;
        }
        bytes = *length;
      }
      entry.parts.push_back({part, bytes, live});
      entry.bytes += bytes;
    }
    if (entry.parts.empty()) continue;
    listing.bytes += entry.bytes;
    listing.boots.push_back(std::move(entry));
  }
  return listing;
}

std::string LogDownload::partPath(std::uint32_t boot, std::uint32_t part) const {
  return layout::partPath(card_.rootDirectory(), boot, part);
}

LogDownload::Outcome LogDownload::start(const DownloadSelection& selection) {
  stop();
  selection_ = selection;
  problem_.clear();
  partsGone_ = 0;
  const std::lock_guard<std::mutex> lock(card_.mountLock());
  const DownloadCard::State state = card_.state();
  if (!state.mounted) return fail("there is no card in the bike, or it isn't mounted");
  generation_ = state.mountGeneration;
  live_ = state.live;
  if (selection.boot) {
    boots_ = {*selection.boot};
  } else {
    std::optional<std::vector<std::uint32_t>> boots = layout::numberedEntries(card_.rootDirectory(), "", "");
    if (!boots) return fail("the card's " + card_.rootDirectory() + " can't be listed");
    boots_ = std::move(*boots);
  }
  measuring_ = true;
  return Outcome::kMeasuring;
}

LogDownload::Outcome LogDownload::measureNext() {
  if (!measuring_) return ready_ ? Outcome::kReady : Outcome::kNothing;
  const std::lock_guard<std::mutex> lock(card_.mountLock());
  if (!sameMount()) return fail("the card was unmounted while the download was being planned");
  if (bootParts_.empty()) {
    if (boots_.empty()) return finish();
    listedBoot_ = boots_.back();
    boots_.pop_back();
    std::optional<std::vector<std::uint32_t>> parts = layout::numberedEntries(
        layout::bootDirectory(card_.rootDirectory(), listedBoot_), layout::kPartPrefix, layout::kPartSuffix);
    if (!parts) {
      if (selection_.boot) {
        measuring_ = false;
        problem_ = "the card holds no boot " + std::to_string(listedBoot_);
        return Outcome::kNothing;
      }
      return Outcome::kMeasuring;  // warned; the other boots still go
    }
    bootParts_ = std::move(*parts);
    // The writer's next part is zeros it may be filling right now: never part of the log.
    if (live_ && listedBoot_ == live_->boot) {
      bootParts_.erase(std::upper_bound(bootParts_.begin(), bootParts_.end(), live_->part), bootParts_.end());
    }
    return Outcome::kMeasuring;
  }
  const std::uint32_t part = bootParts_.back();
  bootParts_.pop_back();
  const std::optional<std::uint64_t> length = measure(listedBoot_, part);
  if (!length) return Outcome::kFailed;
  if (*length == 0) return Outcome::kMeasuring;
  if (*length > kMaxDownloadBytes) {
    logWarning(kTag, "%s is %llu bytes, too big to be a ride-log part; leaving it out", partPath(listedBoot_, part).c_str(),
               static_cast<unsigned long long>(*length));
    return Outcome::kMeasuring;
  }
  // Older parts stop here: the download is the newest parts, with nothing missing between them.
  if (!parts_.empty() && total_ + *length > selection_.maxBytes) return finish();
  parts_.push_back({listedBoot_, part, *length, 0});
  total_ += *length;
  return Outcome::kMeasuring;
}

std::optional<std::uint64_t> LogDownload::measure(std::uint32_t boot, std::uint32_t part) {
  if (live_ && boot == live_->boot && part == live_->part) return live_->durableBytes;
  const std::string path = partPath(boot, part);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return 0;  // deleted to make room since it was listed
    fail(describeErrno("open", path));
    return std::nullopt;
  }
  std::string problem;
  const std::optional<std::uint64_t> length = writtenLength(fd, problem);
  ::close(fd);
  if (!length) fail(path + ": " + problem);
  return length;
}

LogDownload::Outcome LogDownload::finish() {
  measuring_ = false;
  std::reverse(parts_.begin(), parts_.end());
  std::uint64_t offset = 0;
  for (Part& part : parts_) {
    part.offset = offset;
    offset += part.length;
  }
  if (parts_.empty()) {
    problem_ = selection_.boot ? "boot " + std::to_string(*selection_.boot) + " holds no records"
                               : "the card holds no ride log yet";
    return Outcome::kNothing;
  }
  ready_ = true;
  logInfo(kTag, "download: %zu part(s), %llu bytes, %s", parts_.size(), static_cast<unsigned long long>(total_),
          fileName().c_str());
  return Outcome::kReady;
}

LogDownload::Outcome LogDownload::fail(std::string problem) {
  measuring_ = false;
  ready_ = false;
  problem_ = std::move(problem);
  logWarning(kTag, "download: %s", problem_.c_str());
  return Outcome::kFailed;
}

std::string LogDownload::fileName() const {
  if (parts_.empty()) return "emcool.log";
  char name[64];
  const Part& first = parts_.front();
  const Part& last = parts_.back();
  if (parts_.size() == 1) {
    std::snprintf(name, sizeof name, "emcool-%05u.%u.log", static_cast<unsigned>(first.boot),
                  static_cast<unsigned>(first.part));
  } else {
    std::snprintf(name, sizeof name, "emcool-%05u.%u-%05u.%u.log", static_cast<unsigned>(first.boot),
                  static_cast<unsigned>(first.part), static_cast<unsigned>(last.boot), static_cast<unsigned>(last.part));
  }
  return name;
}

// Under the card's lock.
bool LogDownload::sameMount() {
  const DownloadCard::State state = card_.state();
  return state.mounted && state.mountGeneration == generation_;
}

// Under the card's lock, on the same mount.
void LogDownload::closeFile() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  fileIndex_ = kNoFile;
  fileGone_ = false;
}

// Under the card's lock, on the same mount.
bool LogDownload::openFile(std::size_t index) {
  closeFile();
  const Part& part = parts_[index];
  const std::string path = partPath(part.boot, part.part);
  fileIndex_ = index;
  filePosition_ = 0;
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ >= 0) return true;
  if (errno == ENOENT) {
    // Deleted to make room since it was measured. Zeros keep every later offset where the head said.
    fileGone_ = true;
    partsGone_++;
    logWarning(kTag, "download: %s was deleted to make room before it was sent; it goes as zeros", path.c_str());
    return true;
  }
  problem_ = describeErrno("open", path);
  return false;
}

std::size_t LogDownload::read(std::uint64_t offset, std::span<char> out) {
  if (!ready_ || readFailed_ || offset >= total_) return 0;
  const std::lock_guard<std::mutex> lock(card_.mountLock());
  if (!sameMount()) {
    // The descriptor went with the card; another file may have its number now.
    fd_ = -1;
    fileIndex_ = kNoFile;
    readFailed_ = true;
    problem_ = "the card was unmounted during the download";
    logWarning(kTag, "download: %s", problem_.c_str());
    return 0;
  }
  std::size_t filled = 0;
  while (filled < out.size() && offset + filled < total_) {
    const std::uint64_t at = offset + filled;
    // The last part starting at or before `at`.
    const auto next = std::upper_bound(parts_.begin(), parts_.end(), at,
                                       [](std::uint64_t value, const Part& part) { return value < part.offset; });
    const std::size_t index = static_cast<std::size_t>(next - parts_.begin()) - 1;
    const Part& part = parts_[index];
    const std::uint64_t within = at - part.offset;
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - filled, part.length - within));
    if (fileIndex_ != index && !openFile(index)) {
      readFailed_ = true;
      break;
    }
    if (fileGone_) {
      std::memset(out.data() + filled, 0, wanted);
      filled += wanted;
      continue;
    }
    if (filePosition_ != within) {
      if (lseek(fd_, static_cast<off_t>(within), SEEK_SET) < 0) {
        problem_ = describeErrno("seek in", partPath(part.boot, part.part));
        readFailed_ = true;
        break;
      }
      filePosition_ = within;
    }
    const ssize_t got = ::read(fd_, out.data() + filled, wanted);
    if (got <= 0) {
      problem_ = got < 0 ? describeErrno("read", partPath(part.boot, part.part))
                         : partPath(part.boot, part.part) + " is shorter than when the download measured it";
      readFailed_ = true;
      break;
    }
    filePosition_ += static_cast<std::uint64_t>(got);
    filled += static_cast<std::size_t>(got);
    if (filePosition_ == part.length) closeFile();
  }
  if (readFailed_) {
    closeFile();
    logWarning(kTag, "download: %s", problem_.c_str());
  } else if (offset + filled == total_) {
    closeFile();
  }
  return filled;
}

void LogDownload::stop() {
  if (fd_ >= 0 || fileGone_) {
    const std::lock_guard<std::mutex> lock(card_.mountLock());
    if (sameMount()) closeFile();
    fd_ = -1;
    fileIndex_ = kNoFile;
    fileGone_ = false;
  }
  measuring_ = false;
  ready_ = false;
  readFailed_ = false;
  boots_.clear();
  bootParts_.clear();
  parts_.clear();
  total_ = 0;
  live_.reset();
}

}  // namespace emcool::ride_log
