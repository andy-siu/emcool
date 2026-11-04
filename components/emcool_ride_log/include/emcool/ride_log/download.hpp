#pragma once

// The ride log off the card as one file, for the dashboard's /dl over Bluetooth: the chosen parts,
// oldest first, each cut where its records end, so the zero-filled rest of a part never goes over
// the radio. Every record frames itself and starts on a sector boundary, so the parts laid end to
// end read exactly as the card's own files do: tools/import-ride-log.mts imports the download as
// it is.
//
// The Bluetooth task runs it while the writer task goes on writing. The part being written is read
// only as far as the writer says is durable, and the part it is preparing next (zeros) is left out.
// start() lists the boots; measureNext() then lists one boot or measures one part per call, so the
// link never waits long; read() hands the body out in order.
//
// What it can't promise: a part the writer deletes to make room while the download is reading it.
// That only happens to the OLDEST part on a full card, so only to a download that reaches back that
// far. Deleted before its turn, the part goes as zeros (partsGone() counts it). Deleted mid-read,
// what comes over for the rest of it is whatever its freed clusters then hold: zeros, or records the
// writer has since put there. Either way the laptop is not misled: zeros and torn records are
// stepped over by their CRC, and a record that arrives twice is dropped by its session and index.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/ride_log/log_writer.hpp"

namespace emcool::ride_log {

inline constexpr std::uint64_t kMegabyte = 1024 * 1024;
// Roughly the last 40 minutes of riding (at 120-170 readings a second), and about a quarter
// of that over the air once the link has compressed it.
inline constexpr std::uint64_t kDefaultDownloadBytes = 16 * kMegabyte;
// The most one download carries: the phone holds all of it in memory before it saves it.
inline constexpr std::uint64_t kMaxDownloadBytes = 256 * kMegabyte;

// Which parts: every boot's, or one boot's. The newest parts are taken first, a whole part at a
// time, until the next one would take the download past `maxBytes`; at least one part always goes.
struct DownloadSelection {
  std::optional<std::uint32_t> boot;
  std::uint64_t maxBytes = kDefaultDownloadBytes;
};

// What the download needs from the card and its writer.
class DownloadCard {
 public:
  struct State {
    bool mounted = false;
    // Goes up each time the card is unmounted. A file opened before that is gone, and its
    // descriptor number may since have gone to another file, so it is forgotten, never closed.
    std::uint32_t mountGeneration = 0;
    // The part being written; nullopt when nothing is.
    std::optional<LiveExtent> live;
  };

  virtual ~DownloadCard() = default;
  virtual const std::string& rootDirectory() const = 0;
  // Held around state() and every file operation; the card is never unmounted while it is held.
  virtual std::mutex& mountLock() = 0;
  virtual State state() = 0;
};

// How much of a finished part holds records. Records fill a part from its start, every sector of a
// record holds a non-zero byte (the magic, or JSON text, which has no NUL in it), and after the
// last record the part is zeros. So the log ends at the first all-zero sector, and bisection finds
// it in about log2(sectors) reads — 14 for an 8 MB part. Past that boundary it then probes 1, 2,
// 4, … sectors on, so records after a run of sectors the card zeroed are usually still found; any
// it misses are lost from the download, not from the card. Stale data past the end only makes it
// longer, which the laptop steps over. nullopt, with `problem` set, if the file can't be read.
std::optional<std::uint64_t> writtenLength(int fd, std::string& problem);

// What the card holds, for the ride-log page: every boot, newest first, and its parts in order, with
// the size each would download at. Measuring reads each part's end (a few sector reads apiece), so a
// full card's listing takes a moment. The part the writer is preparing next is left out.
struct CardListing {
  struct Part {
    std::uint32_t part;
    std::uint64_t bytes;  // to its last record, as /dl would send it; 0 for one with none yet
    bool live;            // the part being written
  };
  struct Boot {
    std::uint32_t boot;
    std::vector<Part> parts;
    std::uint64_t bytes;
  };
  std::vector<Boot> boots;
  std::uint64_t bytes = 0;
};
// nullopt, with `problem` set, if there is no card or it can't be read.
std::optional<CardListing> listCard(DownloadCard& card, std::string& problem);

class LogDownload {
 public:
  enum class Outcome : std::uint8_t { kMeasuring, kReady, kNothing, kFailed };

  explicit LogDownload(DownloadCard& card) : card_(card) {}
  ~LogDownload() { stop(); }
  LogDownload(const LogDownload&) = delete;
  LogDownload& operator=(const LogDownload&) = delete;

  // Ends any download in progress and starts planning this one: kMeasuring, or kNothing / kFailed
  // with problem() saying why.
  Outcome start(const DownloadSelection& selection);
  // One more step of the plan: kMeasuring until it is complete, then kReady — or kNothing if no
  // chosen part holds a record, or kFailed.
  Outcome measureNext();

  std::uint64_t totalBytes() const { return total_; }
  std::size_t partCount() const { return parts_.size(); }
  // "emcool-00012.3-00014.1.log": the first and last part sent, as boot.part.
  std::string fileName() const;
  // Why start() or measureNext() said kNothing or kFailed, or why read() came up short.
  const std::string& problem() const { return problem_; }
  // Parts deleted after they were measured; their bytes came over as zeros.
  std::uint32_t partsGone() const { return partsGone_; }

  // Up to out.size() bytes of the body from `offset`, once kReady. Fewer only at the end, or after
  // an error, which problem() names; nothing more is read after an error.
  std::size_t read(std::uint64_t offset, std::span<char> out);
  // Closes the file it holds and forgets the plan.
  void stop();

 private:
  struct Part {
    std::uint32_t boot;
    std::uint32_t part;
    std::uint64_t length;
    std::uint64_t offset;  // where it starts in the body
  };
  static constexpr std::size_t kNoFile = static_cast<std::size_t>(-1);

  Outcome finish();
  Outcome fail(std::string problem);
  bool sameMount();
  void closeFile();
  bool openFile(std::size_t index);
  std::optional<std::uint64_t> measure(std::uint32_t boot, std::uint32_t part);
  std::string partPath(std::uint32_t boot, std::uint32_t part) const;

  DownloadCard& card_;
  DownloadSelection selection_;
  std::uint32_t generation_ = 0;
  std::optional<LiveExtent> live_;
  bool measuring_ = false;
  bool ready_ = false;
  // Boots still to list, ascending, taken from the back; the listed boot and its parts, likewise.
  std::vector<std::uint32_t> boots_;
  std::uint32_t listedBoot_ = 0;
  std::vector<std::uint32_t> bootParts_;
  // Measured newest first, then turned round into the plan.
  std::vector<Part> parts_;
  std::uint64_t total_ = 0;
  std::string problem_;
  std::uint32_t partsGone_ = 0;

  // The part being read: its descriptor, or `fileGone_` if it was deleted after it was measured.
  std::size_t fileIndex_ = kNoFile;
  int fd_ = -1;
  bool fileGone_ = false;
  std::uint64_t filePosition_ = 0;
  bool readFailed_ = false;
};

}  // namespace emcool::ride_log
