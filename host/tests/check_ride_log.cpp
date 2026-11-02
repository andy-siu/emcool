// The ride log on the host: record framing, the sealing rules, and the writer's files.
//
//   check_ride_log                                     unit checks
//   check_ride_log --corpus <golden> <dir> <expected> [--card-bytes N | --statvfs]
//       run the decoder corpus through the whole pipeline into <dir>/RIDES, and write every
//       reading the log was given to <expected> (JSON lines) for tools/check-ride-log.mts to
//       compare against what the laptop reads back. With --card-bytes the card is simulated
//       at N bytes; with --statvfs <dir> is a real (small) filesystem. Either way the card
//       fills and the oldest parts must be rotated away.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sys/statvfs.h>

#include "check.hpp"
#include "emcool/can/frame_decoder.hpp"
#include "emcool/clock/system_clock_sync.hpp"
#include "emcool/ride_log/crc32.hpp"
#include "emcool/ride_log/log_writer.hpp"
#include "emcool/ride_log/record.hpp"
#include "emcool/ride_log/ride_log.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/signals/registry.hpp"
#include "emcool/signals/signal_store.hpp"

namespace fs = std::filesystem;
using emcool::clock::ClockTrust;
using emcool::ride_log::SegmentBuffer;

namespace {

class SettableTrust final : public emcool::clock::ClockTrustSource {
 public:
  ClockTrust trust() const override { return current; }
  ClockTrust current = ClockTrust::kNeverSynced;
};

// Takes sealed buffers without writing them; `release` hands them back as the writer would.
class HoldingQueue final : public emcool::ride_log::SegmentQueue {
 public:
  bool submit(SegmentBuffer& buffer) override {
    submitted.push_back(&buffer);
    return true;
  }
  void releaseAll() {
    for (SegmentBuffer* buffer : submitted) {
      buffer->heldByWriter.store(false);
    }
  }
  std::vector<SegmentBuffer*> submitted;
};

// Writes each sealed segment immediately, then gives the writer `idleSteps` background steps.
class WritingQueue final : public emcool::ride_log::SegmentQueue {
 public:
  explicit WritingQueue(emcool::ride_log::LogWriter& writer) : writer_(writer) {}
  bool submit(SegmentBuffer& buffer) override {
    if (!writer_.write(buffer)) {
      writeFailures++;
    }
    buffer.heldByWriter.store(false);
    for (int step = 0; step < idleSteps; step++) {
      writer_.prepareAhead();
    }
    return true;
  }
  int idleSteps = 0;
  int writeFailures = 0;

 private:
  emcool::ride_log::LogWriter& writer_;
};

// A card of `capacity` bytes: free is capacity minus every file stored under `root`.
class SimulatedCard final : public emcool::ride_log::CardSpace {
 public:
  SimulatedCard(fs::path root, std::uint64_t capacity) : root_(std::move(root)), capacity_(capacity) {}
  std::optional<std::uint64_t> freeBytes() override {
    const std::uint64_t used = usedBytes();
    return used >= capacity_ ? 0 : capacity_ - used;
  }
  std::uint64_t usedBytes() const {
    std::uint64_t used = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root_)) {
      if (entry.is_regular_file()) used += entry.file_size();
    }
    return used;
  }
  std::uint64_t capacity() const { return capacity_; }

 private:
  fs::path root_;
  std::uint64_t capacity_;
};

// The free space of the real filesystem under `root` — a small FAT image in tools/check.sh.
class StatvfsCard final : public emcool::ride_log::CardSpace {
 public:
  explicit StatvfsCard(std::string root) : root_(std::move(root)) {}
  std::optional<std::uint64_t> freeBytes() override {
    struct statvfs info = {};
    if (statvfs(root_.c_str(), &info) != 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
  }

 private:
  std::string root_;
};

// Every file under `root`, relative to it.
std::set<std::string> filesUnder(const fs::path& root) {
  std::set<std::string> files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) files.insert(fs::relative(entry.path(), root).string());
  }
  return files;
}

void writeBytes(const fs::path& path, std::size_t count) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  const std::vector<char> bytes(count, '\x5a');
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const emcool::signals::SignalDef& signalNamed(const char* key) {
  return emcool::signals::signalRegistry()[*emcool::signals::findSignal(key)];
}

emcool::ride_log::SessionId testSession() {
  return emcool::ride_log::SessionId::fromBytes({0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef});
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::string payloadText(const emcool::ride_log::ScannedRecord& record) {
  return std::string(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
}

std::size_t countRows(const std::string& payload) {
  std::size_t rows = 0;
  std::istringstream lines(payload);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line[0] == '[') rows++;
  }
  return rows;
}

void checkCrc32();
void checkNumbersRoundTrip();
void checkSealingRules();
void checkDropsWhenWriterIsBehind();
void checkWriterFiles();
void checkRotation();
void checkFullCardOfForeignFiles();
int runCorpus(const char* goldenPath, const char* directory, const char* expectedPath, const char* cardOption,
              const char* cardValue);

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount >= 5 && std::strcmp(arguments[1], "--corpus") == 0) {
    return runCorpus(arguments[2], arguments[3], arguments[4], argumentCount > 5 ? arguments[5] : nullptr,
                     argumentCount > 6 ? arguments[6] : nullptr);
  }
  if (argumentCount != 1) {
    std::fprintf(stderr, "usage: check_ride_log [--corpus <golden> <dir> <expected>]\n");
    return 2;
  }
  checkCrc32();
  checkNumbersRoundTrip();
  checkSealingRules();
  checkDropsWhenWriterIsBehind();
  checkWriterFiles();
  checkRotation();
  checkFullCardOfForeignFiles();
  std::printf("check_ride_log: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}

namespace {

void checkCrc32() {
  const char* text = "123456789";
  const auto crc = emcool::ride_log::crc32(reinterpret_cast<const std::uint8_t*>(text), 9);
  EMCOOL_CHECK(crc == 0xCBF43926u, "crc32(\"123456789\") = %08x, the standard check value is cbf43926", crc);
  const auto first = emcool::ride_log::crc32(reinterpret_cast<const std::uint8_t*>(text), 4);
  const auto continued = emcool::ride_log::crc32(reinterpret_cast<const std::uint8_t*>(text) + 4, 5, first);
  EMCOOL_CHECK(continued == crc, "a continued CRC differs: %08x", continued);
}

void checkNumbersRoundTrip() {
  const double values[] = {0.1, -0.0, 1e21, 5e-324, 1.7976931348623157e308, 123456789012345680.0, -408.607,
                           0.00003, 3.3e-7, 4294967295.0, -2.5, 1767225600.123};
  for (const double value : values) {
    char text[64];
    emcool::text::TextBuffer buffer(text, sizeof text - 1);
    buffer.appendNumber(value);
    text[buffer.length()] = '\0';
    const double parsed = std::strtod(text, nullptr);
    // -0 is written "0", as cool-eva's JSON.stringify writes it; everything else keeps its sign.
    const bool negativeZero = value == 0 && std::signbit(value);
    EMCOOL_CHECK(!buffer.overflowed() && parsed == value &&
                     (negativeZero ? std::string(text) == "0" : std::signbit(parsed) == std::signbit(value)),
                 "%.17g wrote as \"%s\"", value, text);
  }
}

void checkSealingRules() {
  SettableTrust trust;
  HoldingQueue queue;
  SegmentBuffer buffers[2];
  emcool::ride_log::RideLog log(testSession(), buffers, queue, trust);
  const auto& soc = signalNamed("soc");
  const auto& state = signalNamed("vehicle_state_can");

  log.appendReading(1000, 0, soc, 50);
  log.tick(4999);
  EMCOOL_CHECK(queue.submitted.empty(), "a segment sealed before it was 5 s old");
  log.tick(5000);
  EMCOOL_CHECK(queue.submitted.size() == 1, "a 5 s old segment was not sealed (%zu)", queue.submitted.size());
  queue.releaseAll();

  // Trust change: the rows before it go out under the old trust.
  log.appendReading(6000, 6000, soc, 51);
  trust.current = ClockTrust::kSatelliteBacked;
  log.appendReading(7000, 7000, soc, 52);
  EMCOOL_CHECK(queue.submitted.size() == 2, "a trust change did not seal (%zu)", queue.submitted.size());
  EMCOOL_CHECK(queue.submitted[1]->trust == ClockTrust::kNeverSynced, "the sealed segment has the new trust");
  log.seal();
  EMCOOL_CHECK(queue.submitted.size() == 3 && queue.submitted[2]->trust == ClockTrust::kSatelliteBacked,
               "the next segment did not carry the new trust");
  queue.releaseAll();

  // Parking: entering 60 from another state seals; staying, or re-entering within 5 s, does not.
  const std::size_t beforePark = queue.submitted.size();
  log.appendReading(8000, 8000, state, 60);
  EMCOOL_CHECK(queue.submitted.size() == beforePark, "the first state ever seen sealed as a park");
  log.appendReading(8100, 8100, state, 20);
  log.appendReading(8200, 8200, state, 60);
  EMCOOL_CHECK(queue.submitted.size() == beforePark + 1, "entering park did not seal");
  queue.releaseAll();
  log.appendReading(8300, 8300, state, 20);
  log.appendReading(8400, 8400, state, 60);
  EMCOOL_CHECK(queue.submitted.size() == beforePark + 1, "a second park within 5 s sealed again");
  log.appendReading(13300, 13300, state, 20);
  log.appendReading(13300, 13300, state, 60);
  EMCOOL_CHECK(queue.submitted.size() == beforePark + 2, "a park 5 s after the last park seal did not seal");
  queue.releaseAll();

  // Full: rows never split across records, and a full segment seals by itself.
  const std::size_t beforeFill = queue.submitted.size();
  for (int row = 0; row < 400; row++) {
    log.appendReading(20000 + row, 20000, soc, row);
    queue.releaseAll();
  }
  EMCOOL_CHECK(queue.submitted.size() > beforeFill, "400 rows did not fill a segment");
  for (std::size_t index = beforeFill; index < queue.submitted.size(); index++) {
    EMCOOL_CHECK(queue.submitted[index]->payloadLength <= emcool::ride_log::kMaxPayloadBytes,
                 "a segment overran its payload");
  }
  EMCOOL_CHECK(log.counters().readingsDropped == 0, "%llu readings dropped with the writer keeping up",
               static_cast<unsigned long long>(log.counters().readingsDropped));
}

// With both buffers still at the writer, readings are dropped and counted — and still take a
// sequence number, so the gap shows in the log.
void checkDropsWhenWriterIsBehind() {
  SettableTrust trust;
  HoldingQueue queue;
  SegmentBuffer buffers[2];
  emcool::ride_log::RideLog log(testSession(), buffers, queue, trust);
  const auto& soc = signalNamed("soc");
  log.appendReading(1, 0, soc, 1);
  log.seal();
  log.appendReading(2, 0, soc, 2);
  log.seal();
  log.appendReading(3, 0, soc, 3);
  EMCOOL_CHECK(log.counters().readingsDropped == 1, "dropped %llu, expected 1",
               static_cast<unsigned long long>(log.counters().readingsDropped));
  queue.releaseAll();
  log.appendReading(4, 0, soc, 4);
  log.seal();
  EMCOOL_CHECK(queue.submitted.size() == 3, "%zu segments", queue.submitted.size());
  const std::string payload(queue.submitted[2]->payload(), queue.submitted[2]->payloadLength);
  EMCOOL_CHECK(payload.find(",3]") != std::string::npos, "the row after the drop does not carry seq 3: %s",
               payload.c_str());
}

void checkWriterFiles() {
  const fs::path root = fs::temp_directory_path() / "emcool-check-ride-log";
  fs::remove_all(root);
  fs::create_directories(root / "00001");
  fs::create_directories(root / "00007");
  fs::create_directories(root / "notes");

  constexpr std::uint32_t kPartBytes = 64 * 1024;
  std::vector<std::uint8_t> scratch(emcool::ride_log::kRecordCapacityBytes);
  SimulatedCard roomyCard(root, 1ull << 40);
  emcool::ride_log::LogWriter writer({root.string(), kPartBytes, 8 * 1024}, testSession(), scratch, roomyCard);
  EMCOOL_CHECK(writer.open(), "open failed");
  EMCOOL_CHECK(writer.counters().bootNumber.load() == 8, "boot number %u, expected one past 00007",
               writer.counters().bootNumber.load());

  SettableTrust trust;
  WritingQueue queue(writer);
  queue.idleSteps = 2;
  SegmentBuffer buffers[2];
  emcool::ride_log::RideLog log(testSession(), buffers, queue, trust);
  const auto& soc = signalNamed("soc");
  constexpr int kReadings = 6000;
  for (int reading = 0; reading < kReadings; reading++) {
    log.appendReading(1767225600000LL + reading, reading, soc, reading % 101);
  }
  log.seal();
  while (writer.prepareAhead()) {
  }

  const fs::path bootDirectory = root / "00008";
  std::uint32_t expectedIndex = 0;
  std::size_t rows = 0;
  std::size_t parts = 0;
  for (std::uint32_t part = 1;; part++) {
    char name[16];
    std::snprintf(name, sizeof name, "PART%04u.LOG", part);
    const fs::path path = bootDirectory / name;
    if (!fs::exists(path)) break;
    const std::vector<std::uint8_t> bytes = readFile(path);
    const auto records = emcool::ride_log::scanRecords(bytes);
    if (records.empty()) {
      EMCOOL_CHECK(fs::file_size(path) == kPartBytes, "the prepared next part is %llu bytes",
                   static_cast<unsigned long long>(fs::file_size(path)));
      break;
    }
    parts++;
    EMCOOL_CHECK(fs::file_size(path) == kPartBytes, "%s is %llu bytes, not zero-filled to the part size", name,
                 static_cast<unsigned long long>(fs::file_size(path)));
    EMCOOL_CHECK(payloadText(records[0]).find("\"signals\":{") != std::string::npos,
                 "%s does not open with the signal metadata", name);
    for (const auto& record : records) {
      EMCOOL_CHECK(record.index == expectedIndex, "%s: record index %u, expected %u", name, record.index,
                   expectedIndex);
      expectedIndex = record.index + 1;
      rows += countRows(payloadText(record));
    }
  }
  EMCOOL_CHECK(parts >= 3, "only %zu parts for %d readings; roll-over went unexercised", parts, kReadings);
  EMCOOL_CHECK(rows == kReadings, "%zu rows on disk, %d appended", rows, kReadings);
  EMCOOL_CHECK(queue.writeFailures == 0, "%d write failures", queue.writeFailures);

  emcool::ride_log::LogWriter unwritable({"/dev/null/emcool", kPartBytes, 8 * 1024}, testSession(), scratch, roomyCard);
  EMCOOL_CHECK(!unwritable.open() && !unwritable.isOpen(), "a writer under /dev/null opened");
  fs::remove_all(root);
}

// FIFO on a full card: older boots' parts go first, lowest part first, then this boot's own
// oldest parts. Anything not named like a part, anything outside a numbered boot directory,
// and this boot's current and prepared parts are never touched.
void checkRotation() {
  const fs::path card = fs::temp_directory_path() / "emcool-check-rotation";
  fs::remove_all(card);
  const fs::path rides = card / "RIDES";
  constexpr std::uint32_t kPartBytes = 64 * 1024;
  writeBytes(rides / "00001" / "PART0001.LOG", kPartBytes);
  writeBytes(rides / "00001" / "PART0002.LOG", kPartBytes);
  writeBytes(rides / "00002" / "PART0001.LOG", kPartBytes);
  writeBytes(rides / "00002" / "NOTES.TXT", 1024);
  fs::create_directories(rides / "00003");
  writeBytes(rides / "xyz" / "PART0001.LOG", kPartBytes);
  writeBytes(rides / "README.TXT", 100);
  writeBytes(card / "OTHER.BIN", 100 * 1024);
  const std::set<std::string> foreign = {"RIDES/00002/NOTES.TXT", "RIDES/xyz/PART0001.LOG", "RIDES/README.TXT",
                                         "OTHER.BIN"};

  // Room for the existing files, the 1 MB reserve and a handful of parts.
  SimulatedCard simulated(card, 1600 * 1024);
  std::vector<std::uint8_t> scratch(emcool::ride_log::kRecordCapacityBytes);
  emcool::ride_log::LogWriter writer({rides.string(), kPartBytes, 8 * 1024}, testSession(), scratch, simulated);
  EMCOOL_CHECK(writer.open(), "open failed");
  EMCOOL_CHECK(writer.counters().bootNumber.load() == 4, "boot %u, expected 00004", writer.counters().bootNumber.load());

  SettableTrust trust;
  WritingQueue queue(writer);
  queue.idleSteps = 1;
  SegmentBuffer buffers[2];
  emcool::ride_log::RideLog log(testSession(), buffers, queue, trust);
  const auto& soc = signalNamed("soc");
  std::vector<std::string> deletions;
  std::set<std::string> before = filesUnder(card);
  std::uint64_t mostUsed = 0;
  for (int reading = 0; reading < 40000; reading++) {
    log.appendReading(1767225600000LL + reading, reading, soc, reading % 101);
    if (reading % 50 == 0) {
      const std::set<std::string> after = filesUnder(card);
      for (const std::string& file : before) {
        if (after.count(file) == 0) deletions.push_back(file);
      }
      before = after;
      mostUsed = std::max(mostUsed, simulated.usedBytes());
    }
  }
  log.seal();

  const std::vector<std::string> oldestFirst = {"RIDES/00001/PART0001.LOG", "RIDES/00001/PART0002.LOG",
                                                "RIDES/00002/PART0001.LOG", "RIDES/00004/PART0001.LOG",
                                                "RIDES/00004/PART0002.LOG"};
  EMCOOL_CHECK(deletions.size() >= oldestFirst.size(), "only %zu deletions", deletions.size());
  for (std::size_t index = 0; index < oldestFirst.size() && index < deletions.size(); index++) {
    EMCOOL_CHECK(deletions[index] == oldestFirst[index], "deletion %zu was %s, expected %s", index,
                 deletions[index].c_str(), oldestFirst[index].c_str());
  }
  for (std::size_t index = 1; index < deletions.size(); index++) {
    EMCOOL_CHECK(deletions[index - 1] < deletions[index], "%s was deleted after %s", deletions[index].c_str(),
                 deletions[index - 1].c_str());
  }
  EMCOOL_CHECK(writer.counters().partsRotated.load() >= deletions.size(), "counted %u rotations for %zu deletions",
               writer.counters().partsRotated.load(), deletions.size());
  const std::set<std::string> remaining = filesUnder(card);
  for (const std::string& file : foreign) {
    EMCOOL_CHECK(remaining.count(file) == 1, "%s was deleted", file.c_str());
  }
  EMCOOL_CHECK(!fs::exists(rides / "00001"), "00001 survived losing its last part");
  EMCOOL_CHECK(fs::exists(rides / "00002"), "00002 was removed with a non-part file in it");
  EMCOOL_CHECK(fs::exists(rides / "00003"), "an empty boot directory with no parts was touched");
  std::size_t partFiles = 0;
  for (const std::string& file : filesUnder(card)) {
    if (file.rfind("RIDES/0", 0) == 0 && file.find("/PART") != std::string::npos) partFiles++;
  }
  EMCOOL_CHECK(writer.counters().partsOnCard.load() == partFiles, "partsOnCard is %u, the card holds %zu parts",
               writer.counters().partsOnCard.load(), partFiles);
  EMCOOL_CHECK(mostUsed <= simulated.capacity(), "the card held %llu bytes, over its %llu",
               static_cast<unsigned long long>(mostUsed), static_cast<unsigned long long>(simulated.capacity()));

  // What survives of this boot is the newest readings, contiguous, ending with the last one.
  std::vector<std::uint64_t> sequences;
  for (std::uint32_t part = 1; part < 2000; part++) {
    char name[16];
    std::snprintf(name, sizeof name, "PART%04u.LOG", part);
    const fs::path path = rides / "00004" / name;
    if (!fs::exists(path)) continue;
    const std::vector<std::uint8_t> bytes = readFile(path);
    for (const auto& record : emcool::ride_log::scanRecords(bytes)) {
      std::istringstream lines(payloadText(record));
      for (std::string line; std::getline(lines, line);) {
        if (line.empty() || line[0] != '[') continue;
        sequences.push_back(std::strtoull(line.substr(line.rfind(',') + 1).c_str(), nullptr, 10));
      }
    }
  }
  EMCOOL_CHECK(!sequences.empty() && sequences.back() == 39999, "the newest reading did not survive");
  for (std::size_t index = 1; index < sequences.size(); index++) {
    if (sequences[index] != sequences[index - 1] + 1) {
      EMCOOL_CHECK(false, "surviving readings jump from seq %llu to %llu",
                   static_cast<unsigned long long>(sequences[index - 1]),
                   static_cast<unsigned long long>(sequences[index]));
      break;
    }
  }
  EMCOOL_CHECK(sequences.size() < 40000, "nothing of this boot was rotated away");
  fs::remove_all(card);
}

// A card full of someone else's files: nothing of ours to delete, so logging stays off and
// nothing is touched.
void checkFullCardOfForeignFiles() {
  const fs::path card = fs::temp_directory_path() / "emcool-check-full-card";
  fs::remove_all(card);
  writeBytes(card / "HOLIDAY.MP4", 900 * 1024);
  SimulatedCard simulated(card, 1024 * 1024);
  std::vector<std::uint8_t> scratch(emcool::ride_log::kRecordCapacityBytes);
  emcool::ride_log::LogWriter writer({(card / "RIDES").string(), 64 * 1024, 8 * 1024}, testSession(), scratch,
                                     simulated);
  EMCOOL_CHECK(!writer.open() && !writer.isOpen(), "the writer opened on a card with no room");
  EMCOOL_CHECK(fs::exists(card / "HOLIDAY.MP4") && fs::file_size(card / "HOLIDAY.MP4") == 900 * 1024,
               "a foreign file was touched");
  fs::remove_all(card);
}

// The corpus through the whole pipeline. Time and trust are simulated: frames arrive every
// 0.5 ms; the first third stamps with a 1970 clock (never-synced), then the clock is
// satellite-backed, with a contested stretch in the middle of that.
int runCorpus(const char* goldenPath, const char* directory, const char* expectedPath, const char* cardOption,
              const char* cardValue) {
  std::ifstream golden(goldenPath);
  if (!golden) {
    std::fprintf(stderr, "check_ride_log: cannot open %s\n", goldenPath);
    return 2;
  }
  std::vector<std::string> lines;
  for (std::string line; std::getline(golden, line);) {
    lines.push_back(line);
  }

  // Only RIDES/ is cleared: with --statvfs, <dir> is a mount point.
  fs::create_directories(directory);
  fs::remove_all(fs::path(directory) / "RIDES");
  std::unique_ptr<emcool::ride_log::CardSpace> space;
  bool expectRotation = true;
  if (cardOption != nullptr && std::strcmp(cardOption, "--statvfs") == 0) {
    space = std::make_unique<StatvfsCard>(directory);
  } else if (cardOption != nullptr && std::strcmp(cardOption, "--card-bytes") == 0 && cardValue != nullptr) {
    space = std::make_unique<SimulatedCard>(directory, std::strtoull(cardValue, nullptr, 10));
  } else {
    space = std::make_unique<SimulatedCard>(directory, 1ull << 40);
    expectRotation = false;
  }
  std::vector<std::uint8_t> scratch(emcool::ride_log::kRecordCapacityBytes);
  const auto session = emcool::ride_log::SessionId::fromBytes({0xe1, 0xc0, 0x01, 0x5e, 0x55, 0x10, 0x4e, 0x01});
  emcool::ride_log::LogWriter writer({std::string(directory) + "/RIDES", 256 * 1024, 16 * 1024}, session, scratch,
                                     *space);
  if (!writer.open()) {
    return 2;
  }
  SettableTrust trust;
  WritingQueue queue(writer);
  SegmentBuffer buffers[2];
  emcool::ride_log::RideLog rideLog(session, buffers, queue, trust);

  // Records what the store hands the log, in order, then passes it on.
  class Tee final : public emcool::signals::RideLogSink {
   public:
    Tee(emcool::ride_log::RideLog& log, const SettableTrust& trust, std::ofstream& expected)
        : log_(log), trust_(trust), expected_(expected) {}
    void appendReading(std::int64_t wallClockMs, std::int64_t monotonicMs, const emcool::signals::SignalDef& signal,
                       double value) override {
      char line[256];
      std::snprintf(line, sizeof line, "{\"ts\":%lld,\"key\":\"%s\",\"value\":%.17g,\"seq\":%llu,\"trust\":\"%s\"}\n",
                    static_cast<long long>(wallClockMs), signal.key, value,
                    static_cast<unsigned long long>(sequence_++), emcool::clock::clockTrustName(trust_.trust()));
      expected_ << line;
      log_.appendReading(wallClockMs, monotonicMs, signal, value);
    }

   private:
    emcool::ride_log::RideLog& log_;
    const SettableTrust& trust_;
    std::ofstream& expected_;
    std::uint64_t sequence_ = 0;
  };

  std::ofstream expected(expectedPath);
  expected << "{\"session\":\"" << session.hex.data() << "\"}\n";
  Tee tee(rideLog, trust, expected);
  emcool::signals::SignalStore store(tee);
  emcool::can::FrameDecoder decoder;
  std::uint32_t pseudoRandom = 12345;

  for (std::size_t index = 0; index < lines.size(); index++) {
    const std::int64_t monotonicMs = static_cast<std::int64_t>(index / 2);
    const double progress = static_cast<double>(index) / static_cast<double>(lines.size());
    trust.current = progress < 0.33   ? ClockTrust::kNeverSynced
                    : progress < 0.55 ? ClockTrust::kSatelliteBacked
                    : progress < 0.60 ? ClockTrust::kContested
                                      : ClockTrust::kSatelliteBacked;
    const std::int64_t wallClockMs =
        trust.current == ClockTrust::kNeverSynced ? monotonicMs : 1767225600000LL + 86400000LL * 120 + monotonicMs;
    // Vary how much background time the writer gets, so parts sometimes fill before the next
    // one is ready.
    pseudoRandom = pseudoRandom * 1103515245u + 12345u;
    queue.idleSteps = (pseudoRandom >> 16) % 10 < 3 ? 1 : 0;

    std::istringstream fields(lines[index]);
    std::string idText, payloadHex;
    fields >> idText >> payloadHex;
    std::vector<std::uint8_t> payload;
    if (payloadHex != "-") {
      for (std::size_t offset = 0; offset + 1 < payloadHex.size(); offset += 2) {
        payload.push_back(static_cast<std::uint8_t>(std::strtoul(payloadHex.substr(offset, 2).c_str(), nullptr, 16)));
      }
    }
    emcool::can::DecodedValues decoded;
    decoder.decode(static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 16)), payload, decoded);
    for (const auto& value : decoded) {
      store.record(value.key, value.value, wallClockMs, monotonicMs);
    }
    rideLog.tick(monotonicMs);
  }
  rideLog.seal();

  const auto& counters = rideLog.counters();
  std::printf("check_ride_log --corpus: %llu readings in %llu segments (%llu park seals), %llu records, "
              "%u parts (%u prepared in line, %u rotated away), %llu bytes, %llu dropped, %d write failures\n",
              static_cast<unsigned long long>(counters.readingsAppended),
              static_cast<unsigned long long>(counters.segmentsSealed),
              static_cast<unsigned long long>(counters.parkSeals),
              static_cast<unsigned long long>(writer.counters().recordsWritten.load()),
              writer.counters().partsOpened.load(), writer.counters().partsPreparedInLine.load(),
              writer.counters().partsRotated.load(),
              static_cast<unsigned long long>(writer.counters().bytesWritten.load()),
              static_cast<unsigned long long>(counters.readingsDropped), queue.writeFailures);
  const bool rotatedAsExpected = (writer.counters().partsRotated.load() > 0) == expectRotation;
  const bool clean = counters.readingsDropped == 0 && queue.writeFailures == 0 && counters.parkSeals > 0 &&
                     writer.counters().partsPreparedInLine.load() > 0 && rotatedAsExpected;
  if (!clean) {
    std::fprintf(stderr, "check_ride_log --corpus: expected no drops or failures, park seals and in-line part "
                         "preparation to occur, and rotation %s\n", expectRotation ? "to occur" : "not to occur");
  }
  return clean ? 0 : 1;
}

}  // namespace
