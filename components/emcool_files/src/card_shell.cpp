#include "emcool/files/card_shell.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "emcool/log.hpp"

namespace emcool::files {

namespace {

constexpr const char* kTag = "card";
constexpr const char* kRides = "RIDES";
constexpr const char* kServiceFolder = "vcu-params";
constexpr std::size_t kCopyBytes = 4096;
// How much of `type` decides a file is text: a NUL in it means it isn't.
constexpr std::size_t kTextProbeBytes = 512;

const std::vector<CardCommand> kCommands = {
    {"dir", "[PATH]", "List a folder on the card: each entry's size and date, folders marked <DIR>, then the free space. "
                      "`dir *.LOG` lists what matches. `ls` is the same."},
    {"ls", "[PATH]", "The same as dir."},
    {"cd", "[FOLDER]", "Change this console's folder on the card: `cd RIDES`, `cd ..`, `cd /`. Alone, back to the top."},
    {"pwd", nullptr, "Print this console's folder on the card."},
    {"type", "FILE", "Print the start of a text file on the card. `cat` is the same."},
    {"cat", "FILE", "The same as type."},
    {"mkdir", "FOLDER", "Make a folder on the card."},
    {"rmdir", "FOLDER", "Remove an empty folder from the card."},
    {"rm", "[-r] PATH...", "Delete files from the card: `rm OLD.TXT`, `rm RIDES/00003/*`. `rm -r FOLDER` deletes a folder "
                           "and everything in it. The ride being recorded is never deleted."},
    {"cp", "FROM... TO", "Copy files on the card: `cp A.TXT B.TXT`, or several into a folder: `cp RIDES/00003/* SAVED`. "
                         "Never overwrites, and never into RIDES."},
    {"mv", "FROM... TO", "Move or rename files or folders on the card. Never overwrites, and never into RIDES."},
};

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t at = 0; at < left.size(); at++) {
    const auto lower = [](char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    };
    if (lower(left[at]) != lower(right[at])) return false;
  }
  return true;
}

// Name order: FAT's names ignore case, so this does too.
bool before(std::string_view left, std::string_view right) {
  const std::size_t shorter = std::min(left.size(), right.size());
  for (std::size_t at = 0; at < shorter; at++) {
    const auto lower = [](char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    };
    const char a = lower(left[at]);
    const char b = lower(right[at]);
    if (a != b) return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
  }
  return left.size() < right.size();
}

bool startsWith(const std::vector<std::string>& whole, const std::vector<std::string>& prefix) {
  if (prefix.size() > whole.size()) return false;
  for (std::size_t at = 0; at < prefix.size(); at++) {
    if (!equalsIgnoringCase(whole[at], prefix[at])) return false;
  }
  return true;
}

bool hasPattern(std::string_view text) { return text.find_first_of("*?") != std::string_view::npos; }

// * matches any run of characters, ? any one; letters match either case, as FAT's names do.
bool matches(std::string_view pattern, std::string_view name) {
  std::size_t p = 0;
  std::size_t n = 0;
  std::size_t starAt = std::string_view::npos;
  std::size_t resumeAt = 0;
  const auto same = [](char a, char b) { return equalsIgnoringCase(std::string_view(&a, 1), std::string_view(&b, 1)); };
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], name[n]))) {
      p++;
      n++;
    } else if (p < pattern.size() && pattern[p] == '*') {
      starAt = p++;
      resumeAt = n;
    } else if (starAt != std::string_view::npos) {
      p = starAt + 1;
      n = ++resumeAt;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') p++;
  return p == pattern.size();
}

bool allDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string sizeText(std::uint64_t bytes) {
  char text[32];
  if (bytes >= 10ull * 1024 * 1024 * 1024) {
    std::snprintf(text, sizeof text, "%llu GB", static_cast<unsigned long long>(bytes / (1024ull * 1024 * 1024)));
  } else if (bytes >= 10ull * 1024 * 1024) {
    std::snprintf(text, sizeof text, "%llu MB", static_cast<unsigned long long>(bytes / (1024ull * 1024)));
  } else if (bytes >= 10ull * 1024) {
    std::snprintf(text, sizeof text, "%llu KB", static_cast<unsigned long long>(bytes / 1024));
  } else {
    std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
  }
  return text;
}

// The time a file was last written, as the card stores it (FAT keeps local time).
std::string dateText(std::int64_t modified) {
  if (modified <= 0) return "                ";
  const std::time_t seconds = static_cast<std::time_t>(modified);
  std::tm parts = {};
  if (gmtime_r(&seconds, &parts) == nullptr) return "                ";
  char text[24];
  std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &parts);
  return text;
}

std::string errorText(int error) {
  switch (error) {
    case ENOENT:
      return "not found";
    case EEXIST:
      return "already exists";
    case ENOTEMPTY:
      return "not empty";
    case EACCES:
    case EPERM:
      return "refused by the card (read-only, or in use)";
    case ENOSPC:
      return "the card is full";
    case EIO:
      return "the card did not answer (EIO)";
    case ENAMETOOLONG:
      return "the name is too long";
    case EINVAL:
      return "not a name the card accepts";
    case ENOTDIR:
      return "not a folder";
    case EISDIR:
      return "is a folder";
    case ENFILE:
    case EMFILE:
      return "too many files are open on the card just now; try again in a moment";
    default:
      return std::strerror(error);
  }
}

}  // namespace

bool CardShellGate::take(const std::string& console, std::string& holder) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!holder_.empty() && holder_ != console) {
    holder = holder_;
    return false;
  }
  holder_ = console;
  return true;
}

void CardShellGate::give(const std::string& console) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (holder_ == console) holder_.clear();
}

CardShell::CardShell(Card& card, CardShellGate& gate, std::string console, std::size_t maxOutputBytes)
    : card_(card), gate_(gate), console_(std::move(console)), maxOutputBytes_(maxOutputBytes) {}

CardShell::~CardShell() {
  std::string ignored;
  cancel(ignored);
}

bool CardShell::handles(std::string_view command) {
  return std::any_of(kCommands.begin(), kCommands.end(), [&](const CardCommand& c) { return command == c.name; });
}

const std::vector<CardCommand>& CardShell::commands() { return kCommands; }

std::string_view CardShell::summary() {
  return "dir [PATH] | cd [FOLDER] | pwd | type FILE | mkdir FOLDER | rmdir FOLDER | rm [-r] PATH... | cp FROM... TO | "
         "mv FROM... TO";
}

// ---------------------------------------------------------------------------------- the card

bool CardShell::locked(const std::function<void()>& operation) {
  const std::lock_guard<std::mutex> lock(card_.lock());
  const CardState state = card_.state();
  if (!state.mounted || state.generation != generation_) {
    // Its descriptors went with the card: forgotten, never closed (their numbers may be reused).
    from_ = -1;
    to_ = -1;
    cardGone_ = true;
    return false;
  }
  operation();
  return true;
}

bool CardShell::open(std::string& output) {
  cardGone_ = false;
  bool mounted = false;
  {
    const std::lock_guard<std::mutex> lock(card_.lock());
    const CardState state = card_.state();
    mounted = state.mounted;
    generation_ = state.generation;
  }
  if (!mounted) {
    fail(output, "no card: the microSD card isn't mounted");
    return false;
  }
  // The folder this console was in may have gone (another console, or the ride log's rotation).
  if (!folder_.empty()) {
    FileInfo info;
    if (!statPath(folder_, info)) {
      fail(output, "the card went away");
      return false;
    }
    if (!info.exists || !info.folder) {
      say(output, shown(folder_) + " is gone; back to /");
      folder_.clear();
    }
  }
  return true;
}

bool CardShell::statPath(const Path& path, FileInfo& info) {
  info = {};
  const std::string full = hostPath(path);
  return locked([&] {
    if (path.empty()) {
      info.exists = true;
      info.folder = true;
      return;
    }
    struct stat status = {};
    if (::stat(full.c_str(), &status) != 0) {
      info.error = errno;
      return;
    }
    info.exists = true;
    info.folder = S_ISDIR(status.st_mode);
    info.bytes = static_cast<std::uint64_t>(status.st_size);
  });
}

bool CardShell::list(const Path& folder, std::string_view pattern, std::string_view after, std::size_t limit,
                     std::vector<Entry>& entries, std::size_t& more, int& error) {
  entries.clear();
  more = 0;
  error = 0;
  const std::string full = hostPath(folder);
  return locked([&] {
    DIR* listing = ::opendir(full.c_str());
    if (listing == nullptr) {
      error = errno;
      return;
    }
    // Only the first `limit` by name are kept, so a folder of thousands needs no more memory than that.
    while (const dirent* found = ::readdir(listing)) {
      const std::string_view name = found->d_name;
      if (name == "." || name == "..") continue;
      if (!pattern.empty() && !matches(pattern, name)) continue;
      if (!after.empty() && !before(after, name)) continue;
      if (entries.size() == limit) {
        if (!before(name, entries.back().name)) {
          more++;
          continue;
        }
        entries.pop_back();
        more++;
      }
      Entry entry;
      entry.name = std::string(name);
      entry.folder = found->d_type == DT_DIR;
      if (found->d_type != DT_DIR && found->d_type != DT_REG) {
        struct stat status = {};
        const std::string child = full + "/" + entry.name;
        if (::stat(child.c_str(), &status) == 0) entry.folder = S_ISDIR(status.st_mode);
      }
      entries.insert(std::upper_bound(entries.begin(), entries.end(), entry,
                                      [](const Entry& a, const Entry& b) { return before(a.name, b.name); }),
                     std::move(entry));
    }
    ::closedir(listing);
  });
}

// ---------------------------------------------------------------------------------- paths

bool CardShell::resolve(std::string_view text, Path& path, std::string& why) const {
  if (text.empty()) {
    why = "a path can't be empty";
    return false;
  }
  if (text.find('\\') != std::string_view::npos) {
    why = "use / between folders, not \\";
    return false;
  }
  if (text.find(':') != std::string_view::npos) {
    why = std::string(text) + ": a name can't hold a :";
    return false;
  }
  path = text.front() == '/' ? Path() : folder_;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t slash = std::min(text.find('/', at), text.size());
    const std::string_view part = text.substr(at, slash - at);
    if (part.empty() || part == ".") {
      // nothing
    } else if (part == "..") {
      if (!path.empty()) path.pop_back();
    } else {
      path.emplace_back(part);
    }
    at = slash + 1;
  }
  return true;
}

std::string CardShell::hostPath(const Path& path) const {
  std::string full = card_.mountPoint();
  for (const std::string& part : path) full += "/" + part;
  return full;
}

std::string CardShell::shown(const Path& path) {
  if (path.empty()) return "/";
  std::string text;
  for (const std::string& part : path) text += "/" + part;
  return text;
}

std::string CardShell::shownPattern(const Path& folder, std::string_view pattern) {
  return (folder.empty() ? std::string() : shown(folder)) + "/" + std::string(pattern);
}

std::optional<std::string> CardShell::refusal(const Path& path, Change change) {
  return refusal(path, change, card_.facts());
}

std::optional<std::string> CardShell::refusal(const Path& path, Change change, const CardFacts& facts) {
  if (path.empty()) {
    return change == Change::kRemove ? std::optional<std::string>("the card's top folder can't be removed")
                                     : std::optional<std::string>("/ is the card's top folder");
  }
  const bool rides = equalsIgnoringCase(path[0], kRides);
  if (rides && path.size() == 1) {
    return change == Change::kRemove
               ? std::optional<std::string>("RIDES is the ride log's folder; remove what's in it instead")
               : std::optional<std::string>("RIDES is the ride log's folder");
  }
  if (rides && facts.rideLogStarting) return "the ride log is starting; try again in a moment";
  if (rides && facts.recordingBoot && allDigits(path[1]) &&
      static_cast<std::uint32_t>(std::strtoul(path[1].c_str(), nullptr, 10)) == *facts.recordingBoot) {
    return shown(Path(path.begin(), path.begin() + 2)) + " is the ride being recorded now";
  }
  if (rides && change == Change::kCreate) {
    return "nothing new goes in RIDES: the ride log numbers what's there and deletes its oldest when the card fills";
  }
  if (equalsIgnoringCase(path[0], kServiceFolder)) {
    return std::string(kServiceFolder) +
           " is service mode's folder, which can have a file open at any moment; list, print or copy from it instead";
  }
  return std::nullopt;
}

void CardShell::markChanged(const Path& path) {
  changedCard_ = true;
  if (!path.empty() && equalsIgnoringCase(path[0], kRides)) changedRideLog_ = true;
}

// ---------------------------------------------------------------------------------- commands

bool CardShell::begin(int argumentCount, const char* const* arguments, std::string& output) {
  if (running()) {
    fail(output, "a card command is still running");
    return true;
  }
  ok_ = true;
  written_ = 0;
  unshown_ = 0;
  operations_ = 0;
  deadline_ = Clock::now() + kStepTime;
  const std::string_view command = argumentCount > 0 ? arguments[0] : "";
  if (command == "pwd") {
    if (argumentCount != 1) {
      fail(output, "usage: pwd");
      return true;
    }
    say(output, shown(folder_));
    return true;
  }
  if (!open(output)) return true;
  if (command == "dir" || command == "ls") return dir(argumentCount, arguments, output);
  if (command == "cd") return cd(argumentCount, arguments, output);
  if (command == "type" || command == "cat") return type(argumentCount, arguments, output);
  if (command == "mkdir") return makeFolder(argumentCount, arguments, output);
  if (command == "rmdir") return removeFolder(argumentCount, arguments, output);
  if (command == "rm") return startRemove(argumentCount, arguments, output);
  if (command == "cp") return startTransfer(Job::kCopy, argumentCount, arguments, output);
  if (command == "mv") return startTransfer(Job::kMove, argumentCount, arguments, output);
  fail(output, "not a card command");
  return true;
}

bool CardShell::dir(int count, const char* const* arguments, std::string& output) {
  if (count > 2) {
    fail(output, "usage: dir [PATH]");
    return true;
  }
  std::string why;
  Path path;
  if (!resolve(count == 2 ? arguments[1] : ".", path, why)) {
    fail(output, why);
    return true;
  }
  std::string pattern;
  if (!path.empty() && hasPattern(path.back())) {
    pattern = path.back();
    path.pop_back();
  }
  if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "* and ? work only in the last part of a path");
    return true;
  }
  FileInfo info;
  if (!statPath(path, info)) return abort(output);
  if (!info.exists) {
    fail(output, shown(path) + ": " + errorText(info.error));
    return true;
  }
  std::vector<Entry> entries;
  std::size_t more = 0;
  int error = 0;
  if (!info.folder) {
    // One file: list just it.
    Entry entry;
    entry.name = path.back();
    entry.bytes = info.bytes;
    entries.push_back(entry);
    path.pop_back();
  } else if (!list(path, pattern, "", kMaxListed, entries, more, error)) {
    return abort(output);
  } else if (error != 0) {
    fail(output, shown(path) + ": " + errorText(error));
    return true;
  }
  say(output, " " + (pattern.empty() ? shown(path) : shownPattern(path, pattern)));
  std::uint32_t files = 0;
  std::uint32_t folders = 0;
  std::uint64_t bytes = 0;
  for (Entry& entry : entries) {
    Path child = path;
    child.push_back(entry.name);
    const std::string full = hostPath(child);
    if (!locked([&] {
          struct stat status = {};
          if (::stat(full.c_str(), &status) == 0) {
            entry.folder = S_ISDIR(status.st_mode);
            entry.bytes = entry.folder ? 0 : static_cast<std::uint64_t>(status.st_size);
            entry.modified = static_cast<std::int64_t>(status.st_mtime);
          }
        })) {
      return abort(output);
    }
    char line[96];
    if (entry.folder) {
      folders++;
      std::snprintf(line, sizeof line, "%s  %12s  ", dateText(entry.modified).c_str(), "<DIR>");
    } else {
      files++;
      bytes += entry.bytes;
      std::snprintf(line, sizeof line, "%s  %12llu  ", dateText(entry.modified).c_str(),
                    static_cast<unsigned long long>(entry.bytes));
    }
    say(output, std::string(line) + entry.name);
  }
  if (more > 0) say(output, "  … and " + std::to_string(more) + " more; narrow it with a pattern, like dir *.LOG");
  std::string totals = "  " + std::to_string(files) + " file" + (files == 1 ? "" : "s") + ", " + sizeText(bytes) +
                       "; " + std::to_string(folders) + " folder" + (folders == 1 ? "" : "s");
  if (more > 0) totals += " (of the first " + std::to_string(entries.size()) + ")";
  say(output, totals);
  if (const std::optional<FreeSpace> space = card_.freeSpace()) {
    say(output, "  " + sizeText(space->freeBytes) + " free of " + sizeText(space->totalBytes));
  }
  return finish(output);
}

bool CardShell::cd(int count, const char* const* arguments, std::string& output) {
  if (count > 2) {
    fail(output, "usage: cd [FOLDER]");
    return true;
  }
  if (count == 1) {
    folder_.clear();
    return finish(output);
  }
  std::string why;
  Path path;
  if (!resolve(arguments[1], path, why)) {
    fail(output, why);
    return true;
  }
  if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "cd takes one folder, not a pattern");
    return true;
  }
  FileInfo info;
  if (!statPath(path, info)) return abort(output);
  if (!info.exists) {
    fail(output, shown(path) + ": " + errorText(info.error));
    return true;
  }
  if (!info.folder) {
    fail(output, shown(path) + " is a file, not a folder");
    return true;
  }
  folder_ = path;
  return finish(output);
}

bool CardShell::type(int count, const char* const* arguments, std::string& output) {
  if (count != 2) {
    fail(output, "usage: type FILE");
    return true;
  }
  std::string why;
  Path path;
  if (!resolve(arguments[1], path, why)) {
    fail(output, why);
    return true;
  }
  if (path.empty() || std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "type takes one file");
    return true;
  }
  const std::string full = hostPath(path);
  const std::size_t room = std::min(kMaxTypeBytes, maxOutputBytes_ > 256 ? maxOutputBytes_ - 256 : maxOutputBytes_);
  std::string text;
  std::uint64_t size = 0;
  int error = 0;
  bool folder = false;
  if (!locked([&] {
        struct stat status = {};
        if (::stat(full.c_str(), &status) != 0) {
          error = errno;
          return;
        }
        if (S_ISDIR(status.st_mode)) {
          folder = true;
          return;
        }
        size = static_cast<std::uint64_t>(status.st_size);
        const int fd = ::open(full.c_str(), O_RDONLY);
        if (fd < 0) {
          error = errno;
          return;
        }
        text.resize(static_cast<std::size_t>(std::min<std::uint64_t>(size, room)));
        std::size_t got = 0;
        while (got < text.size()) {
          const ssize_t now = ::read(fd, text.data() + got, text.size() - got);
          if (now <= 0) break;
          got += static_cast<std::size_t>(now);
        }
        text.resize(got);
        ::close(fd);
      })) {
    return abort(output);
  }
  if (folder) {
    fail(output, shown(path) + " is a folder; dir lists it");
    return true;
  }
  if (error != 0) {
    fail(output, shown(path) + ": " + errorText(error));
    return true;
  }
  if (text.substr(0, kTextProbeBytes).find('\0') != std::string::npos) {
    fail(output, shown(path) + " isn't text (" + sizeText(size) + "); copy it off the card to read it");
    return true;
  }
  // Each line as its own, so a console's line endings stay its own.
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + at, end - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    output.append(line);
    output += '\n';
    at = end + 1;
  }
  if (size > text.size()) {
    say(output, "[the first " + sizeText(text.size()) + " of " + sizeText(size) + "]");
  }
  return finish(output);
}

bool CardShell::makeFolder(int count, const char* const* arguments, std::string& output) {
  if (count != 2) {
    fail(output, "usage: mkdir FOLDER");
    return true;
  }
  std::string why;
  Path path;
  if (!resolve(arguments[1], path, why)) {
    fail(output, why);
    return true;
  }
  if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "a name can't hold * or ?");
    return true;
  }
  if (const std::optional<std::string> refused = refusal(path, Change::kCreate)) {
    fail(output, "not made: " + *refused);
    return true;
  }
  if (!takeGate(output)) return true;
  logChange(count, arguments);
  const std::string full = hostPath(path);
  int error = 0;
  if (!locked([&] {
        if (::mkdir(full.c_str(), 0775) != 0) error = errno;
      })) {
    return abort(output);
  }
  if (error != 0) {
    fail(output, shown(path) + ": " + errorText(error));
  } else {
    markChanged(path);
    foldersDone_++;
  }
  return finish(output);
}

bool CardShell::removeFolder(int count, const char* const* arguments, std::string& output) {
  if (count != 2) {
    fail(output, "usage: rmdir FOLDER");
    return true;
  }
  std::string why;
  Path path;
  if (!resolve(arguments[1], path, why)) {
    fail(output, why);
    return true;
  }
  if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "rmdir takes one folder; rm -r removes several");
    return true;
  }
  if (const std::optional<std::string> refused = refusal(path, Change::kRemove)) {
    fail(output, "not removed: " + *refused);
    return true;
  }
  if (!takeGate(output)) return true;
  logChange(count, arguments);
  const std::string full = hostPath(path);
  int error = 0;
  if (!locked([&] {
        if (::rmdir(full.c_str()) != 0) error = errno;
      })) {
    return abort(output);
  }
  if (error == ENOTEMPTY || error == EEXIST || error == EACCES) {
    // FatFs answers EACCES for a folder that isn't empty, as for a card that's read-only: look.
    std::vector<Entry> inside;
    std::size_t more = 0;
    int listed = 0;
    if (!list(path, "", "", 1, inside, more, listed)) return abort(output);
    fail(output, shown(path) + (listed == 0 && !inside.empty() ? " isn't empty; rm -r removes it and what's in it"
                                                               : ": " + errorText(error)));
  } else if (error != 0) {
    fail(output, shown(path) + ": " + errorText(error));
  } else {
    if (startsWith(folder_, path)) folder_.assign(path.begin(), path.end() - 1);
    markChanged(path);
    foldersDone_++;
  }
  return finish(output);
}

// ---------------------------------------------------------------------------------- the sources

// The paths a command's arguments name, one at a time: a plain path as it is, and a pattern's
// matches a few at a time in name order, listed again after each batch, so a pattern over
// thousands of files needs no more memory than a batch.
bool CardShell::nextSource(Path& source, std::string& output) {
  constexpr std::size_t kBatch = 16;
  while (true) {
    if (pendingAt_ < pending_.size()) {
      source = walkFolder_;
      source.push_back(pending_[pendingAt_++]);
      return true;
    }
    if (walking_) {
      std::vector<Entry> entries;
      std::size_t more = 0;
      int error = 0;
      if (!list(walkFolder_, walkPattern_, walkAfter_, kBatch, entries, more, error)) return false;
      pending_.clear();
      pendingAt_ = 0;
      for (const Entry& entry : entries) pending_.push_back(entry.name);
      if (!pending_.empty()) walkAfter_ = pending_.back();
      walkMatched_ += pending_.size();
      if (error != 0) {
        fail(output, shown(walkFolder_) + ": " + errorText(error));
        walking_ = false;
      } else if (more == 0) {
        walking_ = false;
        if (walkMatched_ == 0) fail(output, shownPattern(walkFolder_, walkPattern_) + ": nothing matches");
      }
      if (!pending_.empty()) continue;
    }
    if (argument_ >= arguments_.size()) return false;
    std::string why;
    Path path;
    if (!resolve(arguments_[argument_++], path, why)) {
      fail(output, why);
      continue;
    }
    if (path.empty() || !hasPattern(path.back())) {
      if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
        fail(output, shown(path) + ": * and ? work only in the last part of a path");
        continue;
      }
      source = path;
      return true;
    }
    walkPattern_ = path.back();
    path.pop_back();
    if (std::any_of(path.begin(), path.end(), [](const std::string& part) { return hasPattern(part); })) {
      fail(output, shown(path) + ": * and ? work only in the last part of a path");
      continue;
    }
    walkFolder_ = path;
    walkAfter_.clear();
    walkMatched_ = 0;
    walking_ = true;
  }
}

// ---------------------------------------------------------------------------------- rm

bool CardShell::startRemove(int count, const char* const* arguments, std::string& output) {
  arguments_.clear();
  recursive_ = false;
  for (int at = 1; at < count; at++) {
    const std::string_view word = arguments[at];
    if (word == "-r" || word == "-R" || word == "-rf" || word == "-fr") {
      recursive_ = true;
    } else if (word.size() > 1 && word[0] == '-') {
      fail(output, "rm knows only -r");
      return true;
    } else {
      arguments_.emplace_back(word);
    }
  }
  if (arguments_.empty()) {
    fail(output, "usage: rm [-r] PATH...");
    return true;
  }
  if (!takeGate(output)) return true;
  logChange(count, arguments);
  argument_ = 0;
  walking_ = false;
  pending_.clear();
  pendingAt_ = 0;
  emptying_.clear();
  job_ = Job::kRemove;
  return stepRemove(output);
}

bool CardShell::stepRemove(std::string& output) {
  while (true) {
    if (!emptying_.empty()) {
      if (!emptyStep(output)) return cardGone_ ? abort(output) : false;
      continue;
    }
    if (!anotherOperation()) return false;
    Path source;
    if (!nextSource(source, output)) return cardGone_ ? abort(output) : finish(output);
    removeSource(source, output);
    if (cardGone_) return abort(output);
  }
}

void CardShell::removeSource(const Path& source, std::string& output) {
  FileInfo info;
  if (!statPath(source, info)) return;
  if (!info.exists) {
    fail(output, shown(source) + ": " + errorText(info.error));
    return;
  }
  if (!info.folder) {
    removeFile(source, output);
    return;
  }
  if (const std::optional<std::string> refused = refusal(source, Change::kRemove)) {
    fail(output, shown(source) + " not deleted: " + *refused);
    return;
  }
  if (!recursive_) {
    fail(output, shown(source) + " is a folder; rm -r deletes it and what's in it, rmdir an empty one");
    return;
  }
  emptying_.push_back({source, "", false});
}

// True when the file went.
bool CardShell::removeFile(const Path& file, std::string& output) {
  // Asked again just before, as service mode may have started since the last file.
  if (const std::optional<std::string> refused = refusal(file, Change::kRemove)) {
    fail(output, shown(file) + " not deleted: " + *refused);
    return false;
  }
  FileInfo info;
  if (!statPath(file, info)) return false;
  const std::string full = hostPath(file);
  int error = 0;
  if (!locked([&] {
        if (::unlink(full.c_str()) != 0) error = errno;
      })) {
    return false;
  }
  if (error != 0) {
    fail(output, shown(file) + ": " + errorText(error));
    return false;
  }
  markChanged(file);
  filesDone_++;
  bytesDone_ += info.bytes;
  return true;
}

// One step into the deepest folder rm -r is emptying: a batch of its entries dealt with, or the
// folder itself removed once nothing is left in it. False when out of time or the card went.
bool CardShell::emptyStep(std::string& output) {
  constexpr std::size_t kBatch = 16;
  const Path folder = emptying_.back().folder;
  const std::string after = emptying_.back().after;
  std::vector<Entry> entries;
  std::size_t more = 0;
  int error = 0;
  if (!list(folder, "", after, kBatch, entries, more, error)) return false;
  if (error != 0) {
    fail(output, shown(folder) + ": " + errorText(error));
    emptying_.pop_back();
    if (!emptying_.empty()) emptying_.back().kept = true;
    return true;
  }
  for (const Entry& entry : entries) {
    if (!anotherOperation()) return false;
    emptying_.back().after = entry.name;
    Path child = folder;
    child.push_back(entry.name);
    if (entry.folder) {
      if (const std::optional<std::string> refused = refusal(child, Change::kRemove)) {
        fail(output, shown(child) + " kept: " + *refused);
        emptying_.back().kept = true;
        continue;
      }
      if (emptying_.size() >= kMaxDepth) {
        fail(output, shown(child) + " kept: more than " + std::to_string(kMaxDepth) + " folders deep");
        emptying_.back().kept = true;
        continue;
      }
      emptying_.push_back({child, "", false});
      return true;
    }
    if (!removeFile(child, output)) {
      if (cardGone_) return false;
      emptying_.back().kept = true;
    }
  }
  if (!entries.empty()) return true;
  // Nothing left in it: the folder goes too, unless something in it had to stay.
  if (emptying_.back().kept) {
    emptying_.pop_back();
    if (!emptying_.empty()) emptying_.back().kept = true;
    return true;
  }
  if (!anotherOperation()) return false;
  if (const std::optional<std::string> refused = refusal(folder, Change::kRemove)) {
    fail(output, shown(folder) + " kept: " + *refused);
    emptying_.pop_back();
    if (!emptying_.empty()) emptying_.back().kept = true;
    return true;
  }
  const std::string full = hostPath(folder);
  int removed = 0;
  if (!locked([&] {
        if (::rmdir(full.c_str()) != 0) removed = errno;
      })) {
    return false;
  }
  emptying_.pop_back();
  if (removed != 0) {
    fail(output, shown(folder) + " kept: " + errorText(removed));
    if (!emptying_.empty()) emptying_.back().kept = true;
    return true;
  }
  markChanged(folder);
  foldersDone_++;
  // The console's own folder may just have gone.
  if (startsWith(folder_, folder)) folder_.assign(folder.begin(), folder.end() - 1);
  return true;
}

// ---------------------------------------------------------------------------------- cp and mv

bool CardShell::startTransfer(Job job, int count, const char* const* arguments, std::string& output) {
  const char* usage = job == Job::kCopy ? "usage: cp FROM... TO" : "usage: mv FROM... TO";
  arguments_.clear();
  for (int at = 1; at < count; at++) {
    if (arguments[at][0] == '-') {
      fail(output, std::string(job == Job::kCopy ? "cp" : "mv") + " takes no options");
      return true;
    }
    arguments_.emplace_back(arguments[at]);
  }
  if (arguments_.size() < 2) {
    fail(output, usage);
    return true;
  }
  std::string why;
  if (!resolve(arguments_.back(), target_, why)) {
    fail(output, why);
    return true;
  }
  arguments_.pop_back();
  if (std::any_of(target_.begin(), target_.end(), [](const std::string& part) { return hasPattern(part); })) {
    fail(output, "the destination can't hold * or ?");
    return true;
  }
  FileInfo info;
  if (!statPath(target_, info)) return abort(output);
  targetIsFolder_ = info.exists && info.folder;
  const bool several = arguments_.size() > 1 || hasPattern(arguments_.front());
  if (several && !targetIsFolder_) {
    fail(output, shown(target_) + (info.exists ? " is a file" : " isn't there") +
                     "; copying or moving several needs a folder to put them in");
    return true;
  }
  if (!takeGate(output)) return true;
  logChange(count, arguments);
  argument_ = 0;
  walking_ = false;
  pending_.clear();
  pendingAt_ = 0;
  job_ = job;
  return job == Job::kCopy ? stepCopy(output) : stepMove(output);
}

CardShell::Path CardShell::destinationFor(const Path& source) const {
  if (!targetIsFolder_) return target_;
  Path destination = target_;
  destination.push_back(source.back());
  return destination;
}

bool CardShell::stepMove(std::string& output) {
  while (true) {
    if (!anotherOperation()) return false;
    Path source;
    if (!nextSource(source, output)) return cardGone_ ? abort(output) : finish(output);
    moveSource(source, output);
    if (cardGone_) return abort(output);
  }
}

void CardShell::moveSource(const Path& source, std::string& output) {
  const CardFacts facts = card_.facts();
  if (source.empty()) {
    fail(output, "the card's top folder can't be moved");
    return;
  }
  const Path destination = destinationFor(source);
  if (const std::optional<std::string> refused = refusal(source, Change::kRemove, facts)) {
    fail(output, shown(source) + " not moved: " + *refused);
    return;
  }
  if (const std::optional<std::string> refused = refusal(destination, Change::kCreate, facts)) {
    fail(output, shown(source) + " not moved: " + *refused);
    return;
  }
  // A folder into itself would cut it off from the card.
  if (destination.size() > source.size() && startsWith(destination, source)) {
    fail(output, shown(source) + " can't go inside itself");
    return;
  }
  FileInfo info;
  if (!statPath(destination, info)) return;
  if (info.exists) {
    fail(output, shown(source) + " not moved: " + shown(destination) + " already exists");
    return;
  }
  FileInfo from;
  if (!statPath(source, from)) return;
  if (!from.exists) {
    fail(output, shown(source) + ": " + errorText(from.error));
    return;
  }
  const std::string fromPath = hostPath(source);
  const std::string toPath = hostPath(destination);
  int error = 0;
  if (!locked([&] {
        if (::rename(fromPath.c_str(), toPath.c_str()) != 0) error = errno;
      })) {
    return;
  }
  if (error != 0) {
    fail(output, shown(source) + ": " + errorText(error));
    return;
  }
  markChanged(source);
  markChanged(destination);
  if (from.folder) {
    foldersDone_++;
  } else {
    filesDone_++;
    bytesDone_ += from.bytes;
  }
}

bool CardShell::stepCopy(std::string& output) {
  while (true) {
    if (to_ >= 0) {
      if (!copyChunk(output)) return cardGone_ ? abort(output) : false;
      continue;
    }
    if (!anotherOperation()) return false;
    Path source;
    if (!nextSource(source, output)) return cardGone_ ? abort(output) : finish(output);
    if (source.empty()) {
      fail(output, "/ is a folder; cp copies files");
      continue;
    }
    const Path destination = destinationFor(source);
    if (const std::optional<std::string> refused = refusal(destination, Change::kCreate)) {
      fail(output, shown(source) + " not copied: " + *refused);
      continue;
    }
    startCopy(source, output);
    if (cardGone_) return abort(output);
  }
}

void CardShell::startCopy(const Path& source, std::string& output) {
  const Path destination = destinationFor(source);
  FileInfo from;
  if (!statPath(source, from)) return;
  if (!from.exists) {
    fail(output, shown(source) + ": " + errorText(from.error));
    return;
  }
  if (from.folder) {
    fail(output, shown(source) + " is a folder; cp copies files (mv moves a folder)");
    return;
  }
  FileInfo to;
  if (!statPath(destination, to)) return;
  if (to.exists) {
    fail(output, shown(source) + " not copied: " + shown(destination) + " already exists");
    return;
  }
  const std::optional<FreeSpace> space = card_.freeSpace();
  if (space && space->freeBytes < from.bytes + keepFreeBytes_) {
    fail(output, shown(source) + " not copied: " + sizeText(from.bytes) + " won't fit and leave " +
                     sizeText(keepFreeBytes_) + " free for the ride log (" + sizeText(space->freeBytes) + " free)");
    return;
  }
  if (!buffer_) buffer_ = std::make_unique<char[]>(kCopyBytes);
  const std::string fromPath = hostPath(source);
  const std::string toPath = hostPath(destination);
  int error = 0;
  if (!locked([&] {
        from_ = ::open(fromPath.c_str(), O_RDONLY);
        if (from_ < 0) {
          error = errno;
          return;
        }
        to_ = ::open(toPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (to_ < 0) {
          error = errno;
          ::close(from_);
          from_ = -1;
        }
      })) {
    return;
  }
  if (error != 0) {
    fail(output, shown(source) + " not copied: " + errorText(error));
    return;
  }
  copySource_ = source;
  copying_ = destination;
  copied_ = 0;
  markChanged(destination);
}

// About 4 KB of the copy, then a check of the time.
bool CardShell::copyChunk(std::string& output) {
  while (true) {
    if (!anotherOperation()) return false;
    ssize_t got = 0;
    ssize_t put = 0;
    int error = 0;
    if (!locked([&] {
          got = ::read(from_, buffer_.get(), kCopyBytes);
          if (got < 0) {
            error = errno;
            return;
          }
          std::size_t written = 0;
          while (written < static_cast<std::size_t>(got)) {
            put = ::write(to_, buffer_.get() + written, static_cast<std::size_t>(got) - written);
            if (put <= 0) {
              error = put < 0 ? errno : ENOSPC;
              return;
            }
            written += static_cast<std::size_t>(put);
          }
        })) {
      return false;
    }
    if (error != 0) {
      fail(output, shown(copySource_) + " not copied: " + errorText(error));
      closeCopy(true);
      return true;
    }
    if (got == 0) {
      int closeError = 0;
      const int to = to_;
      to_ = -1;
      if (!locked([&] {
            if (::close(to) != 0) closeError = errno;
            ::close(from_);
            from_ = -1;
          })) {
        return false;
      }
      if (closeError != 0) {
        fail(output, shown(copySource_) + " not copied: " + errorText(closeError));
        const std::string partial = hostPath(copying_);
        locked([&] { ::unlink(partial.c_str()); });
      } else {
        filesDone_++;
        bytesDone_ += copied_;
      }
      return true;
    }
    copied_ += static_cast<std::uint64_t>(got);
  }
}

void CardShell::closeCopy(bool removePartial) {
  if (from_ < 0 && to_ < 0) return;
  const std::string partial = hostPath(copying_);
  locked([&] {
    if (from_ >= 0) ::close(from_);
    if (to_ >= 0) ::close(to_);
    from_ = -1;
    to_ = -1;
    if (removePartial) ::unlink(partial.c_str());
  });
  from_ = -1;
  to_ = -1;
}

// ---------------------------------------------------------------------------------- the job

bool CardShell::step(std::string& output) {
  deadline_ = Clock::now() + kStepTime;
  operations_ = 0;
  switch (job_) {
    case Job::kRemove:
      return stepRemove(output);
    case Job::kCopy:
      return stepCopy(output);
    case Job::kMove:
      return stepMove(output);
    case Job::kNone:
      return true;
  }
  return true;
}

bool CardShell::anotherOperation() {
  if (operations_ > 0 && (operations_ >= maxOperationsPerStep_ || Clock::now() >= deadline_)) return false;
  operations_++;
  return true;
}

int CardShell::run(int argumentCount, const char* const* arguments, const std::function<void(std::string&)>& flush) {
  std::string output;
  bool done = begin(argumentCount, arguments, output);
  flush(output);
  while (!done) {
    output.clear();
    done = step(output);
    flush(output);
  }
  return ok_ ? 0 : 1;
}

void CardShell::cancel(std::string& output) {
  if (job_ == Job::kNone && !holdsGate_) return;
  const bool copying = to_ >= 0;
  closeCopy(true);
  if (job_ != Job::kNone) {
    fail(output, copying ? "stopped; the part-made copy of " + shown(copySource_) + " was removed" : "stopped");
  }
  job_ = Job::kNone;
  summarize(output);
  emptying_.clear();
  pending_.clear();
  arguments_.clear();
  buffer_.reset();
  if (holdsGate_) {
    gate_.give(console_);
    holdsGate_ = false;
  }
  if (changedCard_) card_.changed(changedRideLog_);
  changedCard_ = false;
  changedRideLog_ = false;
}

bool CardShell::takeGate(std::string& output) {
  std::string holder;
  if (!gate_.take(console_, holder)) {
    fail(output, "the " + holder + " is changing the card; try again when it has finished");
    return false;
  }
  holdsGate_ = true;
  filesDone_ = 0;
  foldersDone_ = 0;
  bytesDone_ = 0;
  changedCard_ = false;
  changedRideLog_ = false;
  return true;
}

void CardShell::summarize(std::string& output) {
  if (filesDone_ == 0 && foldersDone_ == 0) return;
  const char* verb = job_ == Job::kCopy ? "copied" : job_ == Job::kMove ? "moved" : "deleted";
  std::string line = verb;
  if (filesDone_ > 0) {
    line += " " + std::to_string(filesDone_) + " file" + (filesDone_ == 1 ? "" : "s") + " (" + sizeText(bytesDone_) + ")";
  }
  if (foldersDone_ > 0) {
    line += (filesDone_ > 0 ? " and " : " ") + std::to_string(foldersDone_) + " folder" + (foldersDone_ == 1 ? "" : "s");
  }
  say(output, line);
}

bool CardShell::finish(std::string& output) {
  if (job_ != Job::kNone) summarize(output);
  if (unshown_ > 0) {
    output += "[" + std::to_string(unshown_) + " more lines not shown]\n";
    unshown_ = 0;
  }
  job_ = Job::kNone;
  emptying_.clear();
  pending_.clear();
  arguments_.clear();
  buffer_.reset();
  if (holdsGate_) {
    gate_.give(console_);
    holdsGate_ = false;
  }
  if (changedCard_) card_.changed(changedRideLog_);
  changedCard_ = false;
  changedRideLog_ = false;
  return true;
}

bool CardShell::abort(std::string& output) {
  fail(output, "the card went away: it was unmounted or pulled");
  from_ = -1;
  to_ = -1;
  return finish(output);
}

void CardShell::say(std::string& output, std::string_view line) {
  if (written_ + line.size() + 1 > maxOutputBytes_) {
    unshown_++;
    return;
  }
  written_ += line.size() + 1;
  output.append(line);
  output += '\n';
}

void CardShell::fail(std::string& output, std::string_view line) {
  ok_ = false;
  say(output, line);
}

void CardShell::logChange(int count, const char* const* arguments) const {
  std::string line;
  for (int at = 0; at < count; at++) {
    if (at > 0) line += ' ';
    line += arguments[at];
  }
  logInfo(kTag, "from the %s: %s (in %s)", console_.c_str(), line.c_str(), shown(folder_).c_str());
}

}  // namespace emcool::files
