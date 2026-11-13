#pragma once

// The console's commands for the microSD card, which the USB console, the Wi-Fi console and the
// dashboard's Bike console all run: dir (or ls), cd, pwd, type (or cat), mkdir, rmdir, rm, cp and
// mv. Plain POSIX calls, which ESP-IDF maps onto FatFs, so the host checks run them on a folder of
// the laptop's.
//
// Paths are the card's: / is its top folder, folders are separated by /, and each console has its
// own current folder. FAT names ignore case, and so does everything here that compares them. A *
// or ? in the last part of a path matches several entries, worked through a few at a time in name
// order, so a pattern never needs a list of everything it matches.
//
// ⚠️ FatFs here keeps no record of open files (CONFIG_FATFS_FS_LOCK=0), so removing or renaming a
// file something has open corrupts the card. These are refused, because the firmware has files
// open in them or manages them itself:
//   - the card's top folder and RIDES itself: never removed or moved;
//   - the ride being recorded, RIDES/<boot>: the ride log writes its current part there and
//     prepares the next;
//   - anything new in RIDES (mkdir, cp or mv into it): the ride log numbers what's there, and
//     deletes the oldest parts on its own when the card fills;
//   - anything in RIDES while the ride log is starting, before it says which ride it's recording;
//   - anything in vcu-params, service mode's folder, which can have a file open at any moment (a
//     parameter read, a saved reading, the Faults tab's expected list, a write's audit). It can
//     still be listed, printed and copied out of.
// Each is checked again just before each file is changed, since the ride log can start meanwhile.
// Reading is always safe for the card. A file the ride log deletes to make room while it's being
// read (a cp of an old part, a download) reads short, and the card is unharmed.
//
// Only one console changes the card at a time (CardShellGate), so one console's copy is never
// another's rm; the others can still list and print. A copy leaves kKeepFreeBytes free, room for
// the ride log's next part.
//
// Long work goes in slices, so the dashboard's, which runs in the Bluetooth task, never holds the
// link up: begin() starts a command, and step() does about kStepTime of it until it has finished.
// Every file operation holds the card's lock and first checks it is still the card the command
// started on; a descriptor from before an unmount is forgotten, never closed, as the ride-log
// download's are.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emcool::files {

// The mount, read under Card::lock().
struct CardState {
  bool mounted = false;
  // Goes up each time the card is unmounted: a descriptor from before is gone.
  std::uint32_t generation = 0;
};

// What the ride log is doing with the card.
struct CardFacts {
  // RIDES/<n>, the boot folder the ride log is writing; nullopt when it writes none.
  std::optional<std::uint32_t> recordingBoot;
  // The card is mounted but the ride log hasn't opened its part yet (it does within a moment), so
  // the ride being recorded isn't known.
  bool rideLogStarting = false;
};

struct FreeSpace {
  std::uint64_t freeBytes = 0;
  std::uint64_t totalBytes = 0;
};

// What the firmware supplies: the card's mount point, its lock, and what is using it.
class Card {
 public:
  virtual ~Card() = default;
  virtual const std::string& mountPoint() const = 0;
  // Held around state() and every file operation; the card is never unmounted while it is held.
  virtual std::mutex& lock() = 0;
  virtual CardState state() = 0;
  // Both called without lock(); they may take it themselves.
  virtual CardFacts facts() = 0;
  // nullopt when it can't be read.
  virtual std::optional<FreeSpace> freeSpace() = 0;
  // A command changed the card. `rideLog` when it removed or moved something in RIDES, so the ride
  // log can count its parts again. Called without lock().
  virtual void changed(bool rideLog) = 0;
};

// Lets one console at a time change the card.
class CardShellGate {
 public:
  // False, with the console that has it in `holder`, if another console has it.
  bool take(const std::string& console, std::string& holder);
  void give(const std::string& console);

 private:
  std::mutex mutex_;
  std::string holder_;
};

struct CardCommand {
  const char* name;
  const char* hint;  // its arguments, or nullptr
  const char* help;
};

class CardShell {
 public:
  // dir lists at most this many entries, the first by name, and counts the rest.
  static constexpr std::size_t kMaxListed = 100;
  // rm -r goes this many folders deep, no further.
  static constexpr std::size_t kMaxDepth = 16;
  // type prints at most this much: it reads the file whole.
  static constexpr std::size_t kMaxTypeBytes = 4096;
  static constexpr std::uint64_t kKeepFreeBytes = 16ull * 1024 * 1024;
  static constexpr std::chrono::milliseconds kStepTime{25};

  // `console` names it to the others ("USB console", …). `maxOutputBytes` caps what one command
  // prints, which a console holds until it sends it.
  CardShell(Card& card, CardShellGate& gate, std::string console, std::size_t maxOutputBytes);
  CardShell(const CardShell&) = delete;
  CardShell& operator=(const CardShell&) = delete;
  ~CardShell();

  static bool handles(std::string_view command);
  static const std::vector<CardCommand>& commands();
  // The commands on one line, for a console's `help`.
  static std::string_view summary();

  // Starts a command. True when it has finished and `output` is complete; false when step() has
  // more to do.
  bool begin(int argumentCount, const char* const* arguments, std::string& output);
  // About kStepTime of the work; true once the command has finished.
  bool step(std::string& output);
  // Stops the command: closes its files, removes a part-made copy, and lets another console change
  // the card.
  void cancel(std::string& output);
  bool running() const { return job_ != Job::kNone; }
  // Whether the last command did everything it was asked.
  bool succeeded() const { return ok_; }
  // begin(), then step() until the command finishes, handing what it printed to `flush` after each
  // slice. 0 when it succeeded.
  int run(int argumentCount, const char* const* arguments, const std::function<void(std::string&)>& flush);
  // Back to the card's top folder, for a new Wi-Fi client.
  void reset() { folder_.clear(); }

  // For the host checks.
  void setKeepFreeBytes(std::uint64_t bytes) { keepFreeBytes_ = bytes; }
  void setMaxOperationsPerStep(std::size_t operations) { maxOperationsPerStep_ = operations; }

 private:
  using Path = std::vector<std::string>;
  using Clock = std::chrono::steady_clock;
  enum class Job : std::uint8_t { kNone, kRemove, kCopy, kMove };
  // What a command does to a path: takes it away (rm, rmdir, mv's source) or puts something there
  // (mkdir, cp, mv's destination).
  enum class Change : std::uint8_t { kRemove, kCreate };

  struct Entry {
    std::string name;
    bool folder = false;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
  };
  struct FileInfo {
    bool exists = false;
    bool folder = false;
    std::uint64_t bytes = 0;
    int error = 0;
  };
  // A folder rm -r is emptying: the last entry it dealt with, and whether something had to stay.
  struct Level {
    Path folder;
    std::string after;
    bool kept = false;
  };

  bool dir(int count, const char* const* arguments, std::string& output);
  bool cd(int count, const char* const* arguments, std::string& output);
  bool type(int count, const char* const* arguments, std::string& output);
  bool makeFolder(int count, const char* const* arguments, std::string& output);
  bool removeFolder(int count, const char* const* arguments, std::string& output);
  bool startRemove(int count, const char* const* arguments, std::string& output);
  bool startTransfer(Job job, int count, const char* const* arguments, std::string& output);
  bool stepRemove(std::string& output);
  bool stepCopy(std::string& output);
  bool stepMove(std::string& output);
  void removeSource(const Path& source, std::string& output);
  bool removeFile(const Path& file, std::string& output);
  bool emptyStep(std::string& output);
  void moveSource(const Path& source, std::string& output);
  void startCopy(const Path& source, std::string& output);
  bool copyChunk(std::string& output);

  // The path `text` names, from this console's folder.
  bool resolve(std::string_view text, Path& path, std::string& why) const;
  bool nextSource(Path& source, std::string& output);
  Path destinationFor(const Path& source) const;
  std::string hostPath(const Path& path) const;
  static std::string shown(const Path& path);
  static std::string shownPattern(const Path& folder, std::string_view pattern);
  // Why `path` mustn't be changed now, from what the card's facts say at this moment.
  std::optional<std::string> refusal(const Path& path, Change change);
  static std::optional<std::string> refusal(const Path& path, Change change, const CardFacts& facts);
  void markChanged(const Path& path);

  // Runs `operation` under the card's lock, if it is still the card this command started on. False
  // (and cardGone_) when it isn't.
  bool locked(const std::function<void()>& operation);
  bool open(std::string& output);
  bool statPath(const Path& path, FileInfo& info);
  // The first `limit` entries of `folder` by name that match `pattern` (empty: all) and come after
  // `after`, and how many more do. False only when the card went.
  bool list(const Path& folder, std::string_view pattern, std::string_view after, std::size_t limit,
            std::vector<Entry>& entries, std::size_t& more, int& error);

  bool takeGate(std::string& output);
  bool anotherOperation();
  bool finish(std::string& output);
  bool abort(std::string& output);
  void closeCopy(bool removePartial);
  void summarize(std::string& output);
  void say(std::string& output, std::string_view line);
  void fail(std::string& output, std::string_view line);
  void logChange(int count, const char* const* arguments) const;

  Card& card_;
  CardShellGate& gate_;
  std::string console_;
  std::size_t maxOutputBytes_;
  std::uint64_t keepFreeBytes_ = kKeepFreeBytes;
  std::size_t maxOperationsPerStep_ = 256;
  Path folder_;

  // The command being worked through.
  Job job_ = Job::kNone;
  bool ok_ = true;
  bool holdsGate_ = false;
  bool cardGone_ = false;
  std::uint32_t generation_ = 0;
  Clock::time_point deadline_{};
  std::size_t operations_ = 0;
  std::size_t written_ = 0;
  std::size_t unshown_ = 0;
  // Its arguments, and the pattern being walked.
  std::vector<std::string> arguments_;
  std::size_t argument_ = 0;
  std::optional<Change> sourceChange_;
  bool walking_ = false;
  Path walkFolder_;
  std::string walkPattern_;
  std::string walkAfter_;
  std::size_t walkMatched_ = 0;
  std::vector<std::string> pending_;
  std::size_t pendingAt_ = 0;
  // rm -r.
  bool recursive_ = false;
  std::vector<Level> emptying_;
  // cp and mv.
  Path target_;
  bool targetIsFolder_ = false;
  int from_ = -1;
  int to_ = -1;
  Path copySource_;
  Path copying_;
  std::uint64_t copied_ = 0;
  std::unique_ptr<char[]> buffer_;
  // The summary.
  std::uint32_t filesDone_ = 0;
  std::uint32_t foldersDone_ = 0;
  std::uint64_t bytesDone_ = 0;
  bool changedCard_ = false;
  bool changedRideLog_ = false;
};

}  // namespace emcool::files
