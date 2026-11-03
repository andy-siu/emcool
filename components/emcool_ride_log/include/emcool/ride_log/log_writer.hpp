#pragma once

// The writer side of the ride log, run by its own task so a slow card never stalls decoding.
// Plain POSIX calls (open, pwrite, fsync), which ESP-IDF maps onto FatFs and the host onto
// its own filesystem, so the host checks exercise the same code the bike runs.
//
// Layout: <root>/<boot number>/PART<n>.LOG, 8.3 names so FAT needs no long-filename entries.
// Each boot gets its own directory, so the directory sector rewritten by every fsync holds
// only this boot's entries. A part's full size is zero-filled before records are written
// into it in place, so writing a record changes no FAT allocation; the next part is
// zero-filled in the background while the current one fills. Each part opens with records
// carrying every logged signal's unit, group and source, so a part file stands alone.
//
// When the card is full the oldest parts are deleted, first in first out: lowest boot, then
// lowest part, until a new part fits. Only RIDES/<digits>/PART<digits>.LOG files are ever
// deleted, and never the part being written or the one being prepared.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/clock/system_clock_sync.hpp"
#include "emcool/ride_log/record.hpp"
#include "emcool/ride_log/segment.hpp"

namespace emcool::ride_log {

struct LogWriterConfig {
  std::string rootDirectory;
  std::uint32_t partBytes = 8 * 1024 * 1024;
  // Zero-filling happens in steps this size between records, so a pending record waits at
  // most one step.
  std::uint32_t fillStepBytes = 64 * 1024;
};

// How much room is left on the card: esp_vfs_fat_info on the ESP32, statvfs or a simulated
// card on the host.
class CardSpace {
 public:
  virtual ~CardSpace() = default;
  // nullopt when it can't be read; the writer then carries on and relies on ENOSPC.
  virtual std::optional<std::uint64_t> freeBytes() = 0;
};

// The part being written, and how far into it every record is on the card for good (written and
// fsynced). Nothing before durableBytes changes again; anything after it may be mid-write.
struct LiveExtent {
  std::uint32_t boot;
  std::uint32_t part;
  std::uint64_t durableBytes;
};

// Atomics: the writer task updates them, the decode task's status report reads them.
struct LogWriterCounters {
  std::atomic<std::uint64_t> recordsWritten{0};
  std::atomic<std::uint64_t> bytesWritten{0};
  std::atomic<std::uint64_t> writeErrors{0};
  std::atomic<std::uint32_t> partsOpened{0};
  // The next part was not ready when the current one filled, so it was prepared in line.
  std::atomic<std::uint32_t> partsPreparedInLine{0};
  // Old parts deleted to make room.
  std::atomic<std::uint32_t> partsRotated{0};
  // Ride-log parts on the card, every boot's: counted at open(), then kept current.
  std::atomic<std::uint32_t> partsOnCard{0};
  std::atomic<std::uint32_t> bootNumber{0};
  std::atomic<std::uint32_t> currentPart{0};
};

class LogWriter {
 public:
  // `scratch` must hold kRecordCapacityBytes; it builds the metadata records and supplies
  // the zeros for filling.
  LogWriter(LogWriterConfig config, const SessionId& session, std::span<std::uint8_t> scratch, CardSpace& space);
  ~LogWriter();
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  // Creates this boot's directory (one past the highest already on the card) and part 1.
  bool open();
  bool isOpen() const { return current_.fd >= 0; }
  void close();

  // Finalizes the segment's record header and writes it durably. On failure the writer
  // closes itself; open() again starts a fresh boot directory rather than trust a part
  // that just failed.
  bool write(SegmentBuffer& segment);

  // One step of background zero-filling. Returns false when there is nothing left to do.
  bool prepareAhead();

  const LogWriterCounters& counters() const { return counters_; }
  std::uint32_t partBytes() const { return config_.partBytes; }
  const std::string& rootDirectory() const { return config_.rootDirectory; }
  // nullopt while closed. Safe from any task: the download asks it from the Bluetooth task.
  std::optional<LiveExtent> liveExtent() const;

 private:
  struct Part {
    int fd = -1;
    std::uint32_t number = 0;
    std::uint64_t allocatedBytes = 0;
    std::uint64_t writeOffset = 0;
    bool metadataWritten = false;
  };

  bool createPart(Part& part, std::uint32_t number);
  bool makeRoomForPart();
  bool deleteOldestPart();
  bool writeMakingRoom(int fd, const std::uint8_t* data, std::size_t length, std::uint64_t offset);
  bool zeroFillStep(Part& part);
  bool switchToNextPart();
  bool writeMetadataRecords(clock::ClockTrust trust);
  bool writeRecordAtEnd(const std::uint8_t* record, std::size_t length);
  bool fail(const char* operation, const std::string& path);
  std::string bootDirectory() const;
  std::string bootDirectory(std::uint32_t bootNumber) const;
  std::string partPath(std::uint32_t number) const;
  std::string partPath(std::uint32_t bootNumber, std::uint32_t number) const;

  LogWriterConfig config_;
  SessionId session_;
  std::span<std::uint8_t> scratch_;
  CardSpace& space_;
  std::uint32_t bootNumber_ = 0;
  Part current_;
  Part next_;
  bool nextReady_ = false;
  // No room for the next part even after rotating; set until the next open().
  bool nextPartBlocked_ = false;
  std::uint32_t nextRecordIndex_ = 0;
  LogWriterCounters counters_;
  // liveExtent()'s copy, set after each fsync and part switch.
  mutable std::mutex extentMutex_;
  std::optional<LiveExtent> extent_;
  void publishExtent();
};

}  // namespace emcool::ride_log
