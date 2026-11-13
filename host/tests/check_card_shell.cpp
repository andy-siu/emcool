// The console's card commands (emcool/files/card_shell.hpp) on a folder of the laptop's standing in
// for the card: what each prints, what it changes, and above all what it refuses — the ride being
// recorded, anything new in RIDES, vcu-params while service mode has it, a second console — and
// that a card pulled mid-copy leaves nothing half-done behind.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "emcool/files/card_shell.hpp"

using emcool::files::CardFacts;
using emcool::files::CardShell;
using emcool::files::CardShellGate;
using emcool::files::CardState;
namespace fs = std::filesystem;

namespace {

class FakeCard final : public emcool::files::Card {
 public:
  explicit FakeCard(std::string root) : root_(std::move(root)) {}
  const std::string& mountPoint() const override { return root_; }
  std::mutex& lock() override { return mutex_; }
  CardState state() override { return {mounted, generation}; }
  CardFacts facts() override { return facts_; }
  std::optional<emcool::files::FreeSpace> freeSpace() override { return space; }
  void changed(bool rideLog) override {
    changes++;
    if (rideLog) rideLogChanges++;
  }

  bool mounted = true;
  std::uint32_t generation = 1;
  CardFacts facts_;
  std::optional<emcool::files::FreeSpace> space = emcool::files::FreeSpace{1ull << 30, 32ull << 30};
  int changes = 0;
  int rideLogChanges = 0;

 private:
  std::string root_;
  std::mutex mutex_;
};

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

struct Result {
  int status;
  std::string output;
};

// One command line, run to the end as the USB console runs it.
Result run(CardShell& shell, std::vector<std::string> words) {
  std::vector<const char*> arguments;
  for (const std::string& word : words) arguments.push_back(word.c_str());
  std::string all;
  const int status = shell.run(static_cast<int>(arguments.size()), arguments.data(), [&](std::string& piece) { all += piece; });
  return {status, all};
}

void write(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("emcool-card-shell-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  write(root / "RIDES/00001/PART0001.LOG", std::string(5000, 'x'));
  write(root / "RIDES/00001/PART0002.LOG", std::string(3000, 'y'));
  write(root / "RIDES/00002/PART0001.LOG", "old ride");
  write(root / "RIDES/00003/PART0001.LOG", "recording now");
  write(root / "vcu-params/latest.json", "{\"t\":1}");
  write(root / "NOTES.TXT", "first line\r\nsecond line\n");
  write(root / "BIN.DAT", std::string("ab\0cd", 5));

  FakeCard card(root.string());
  card.facts_.recordingBoot = 3;
  CardShellGate gate;
  CardShell usb(card, gate, "USB console", 64 * 1024);
  usb.setKeepFreeBytes(0);

  // --- dir, cd, pwd
  Result r = run(usb, {"dir"});
  EMCOOL_CHECK(r.status == 0 && contains(r.output, "<DIR>") && contains(r.output, "RIDES") && contains(r.output, "NOTES.TXT"),
               "dir: %s", r.output.c_str());
  EMCOOL_CHECK(contains(r.output, "free of"), "dir shows free space: %s", r.output.c_str());
  // Folders and files in name order, FAT-style (case-insensitive).
  EMCOOL_CHECK(r.output.find("BIN.DAT") < r.output.find("NOTES.TXT") && r.output.find("NOTES.TXT") < r.output.find("RIDES"),
               "dir sorted: %s", r.output.c_str());
  r = run(usb, {"cd", "rides/00001"});
  EMCOOL_CHECK(r.status == 0, "cd is case-insensitive: %s", r.output.c_str());
  r = run(usb, {"pwd"});
  EMCOOL_CHECK(r.output == "/rides/00001\n", "pwd: %s", r.output.c_str());
  r = run(usb, {"ls", "*.log"});
  EMCOOL_CHECK(contains(r.output, "PART0001.LOG") && contains(r.output, "PART0002.LOG") && contains(r.output, "2 files"),
               "ls pattern: %s", r.output.c_str());
  r = run(usb, {"cd", ".."});
  r = run(usb, {"pwd"});
  EMCOOL_CHECK(r.output == "/rides\n", "cd ..: %s", r.output.c_str());
  r = run(usb, {"cd", "../../.."});
  r = run(usb, {"pwd"});
  EMCOOL_CHECK(r.output == "/\n", "cd above the top stays at the top: %s", r.output.c_str());
  r = run(usb, {"cd", "NOTES.TXT"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "is a file"), "cd into a file: %s", r.output.c_str());
  r = run(usb, {"cd", "NOPE"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "not found"), "cd missing: %s", r.output.c_str());
  r = run(usb, {"dir", "C:\\"});
  EMCOOL_CHECK(r.status != 0, "backslash refused: %s", r.output.c_str());

  // --- type
  r = run(usb, {"type", "/NOTES.TXT"});
  EMCOOL_CHECK(r.status == 0 && r.output == "first line\nsecond line\n", "type: [%s]", r.output.c_str());
  r = run(usb, {"cat", "BIN.DAT"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "isn't text"), "type binary: %s", r.output.c_str());
  {
    CardShell small(card, gate, "Wi-Fi console", 300);
    write(root / "BIG.TXT", std::string(2000, 'z'));
    r = run(small, {"type", "BIG.TXT"});
    EMCOOL_CHECK(r.status == 0 && contains(r.output, "[the first"), "type caps: %s", r.output.c_str());
    EMCOOL_CHECK(r.output.size() <= 300, "type stays in the cap: %zu", r.output.size());
    r = run(usb, {"type", "BIG.TXT"});
    EMCOOL_CHECK(r.status == 0 && !contains(r.output, "[the first"), "2000 bytes fits: %s", r.output.c_str());
    write(root / "HUGE.TXT", std::string(20000, 'h'));
    r = run(usb, {"type", "HUGE.TXT"});
    EMCOOL_CHECK(r.output.size() < CardShell::kMaxTypeBytes + 200 && contains(r.output, "[the first"),
                 "type reads at most kMaxTypeBytes: %zu", r.output.size());
  }

  // --- mkdir, rmdir
  r = run(usb, {"mkdir", "SAVED"});
  EMCOOL_CHECK(r.status == 0 && fs::is_directory(root / "SAVED"), "mkdir: %s", r.output.c_str());
  r = run(usb, {"mkdir", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "already exists"), "mkdir twice: %s", r.output.c_str());
  r = run(usb, {"mkdir", "RIDES/EXTRA"});
  EMCOOL_CHECK(r.status != 0 && !fs::exists(root / "RIDES/EXTRA"), "no mkdir in RIDES: %s", r.output.c_str());
  r = run(usb, {"rmdir", "RIDES"});
  EMCOOL_CHECK(r.status != 0 && fs::exists(root / "RIDES"), "RIDES can't go: %s", r.output.c_str());
  write(root / "SAVED/keep.txt", "k");
  r = run(usb, {"rmdir", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "isn't empty") && fs::exists(root / "SAVED"), "rmdir not empty: %s",
               r.output.c_str());
  fs::remove(root / "SAVED/keep.txt");

  // --- cp
  r = run(usb, {"cp", "RIDES/00001/PART0001.LOG", "SAVED"});
  EMCOOL_CHECK(r.status == 0 && read(root / "SAVED/PART0001.LOG") == std::string(5000, 'x'), "cp into folder: %s",
               r.output.c_str());
  EMCOOL_CHECK(contains(r.output, "copied 1 file"), "cp summary: %s", r.output.c_str());
  r = run(usb, {"cp", "RIDES/00001/PART0001.LOG", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "already exists"), "cp never overwrites: %s", r.output.c_str());
  r = run(usb, {"cp", "NOTES.TXT", "RIDES/00002"});
  EMCOOL_CHECK(r.status != 0 && !fs::exists(root / "RIDES/00002/NOTES.TXT"), "cp into RIDES refused: %s",
               r.output.c_str());
  r = run(usb, {"cp", "NOTES.TXT", "BIN.DAT", "NOTFOLDER"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "needs a folder"), "cp several to a file: %s", r.output.c_str());
  // A copy may read the recording ride; reading is always safe.
  r = run(usb, {"cp", "RIDES/00003/PART0001.LOG", "NOW.LOG"});
  EMCOOL_CHECK(r.status == 0 && read(root / "NOW.LOG") == "recording now", "cp from the live ride: %s", r.output.c_str());
  // Room for the ride log.
  usb.setKeepFreeBytes(16ull * 1024 * 1024);
  card.space->freeBytes = 16ull * 1024 * 1024 + 100;
  r = run(usb, {"cp", "RIDES/00001/PART0002.LOG", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "free for the ride log") && !fs::exists(root / "SAVED/PART0002.LOG"),
               "cp keeps room: %s", r.output.c_str());
  usb.setKeepFreeBytes(0);
  card.space->freeBytes = 1ull << 30;
  r = run(usb, {"cp", "SAVED", "SAVED2"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "is a folder"), "cp a folder: %s", r.output.c_str());

  // --- mv
  r = run(usb, {"mv", "NOW.LOG", "SAVED/NOW.LOG"});
  EMCOOL_CHECK(r.status == 0 && fs::exists(root / "SAVED/NOW.LOG") && !fs::exists(root / "NOW.LOG"), "mv: %s",
               r.output.c_str());
  r = run(usb, {"mv", "RIDES/00003/PART0001.LOG", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "being recorded") && fs::exists(root / "RIDES/00003/PART0001.LOG"),
               "mv the live ride refused: %s", r.output.c_str());
  r = run(usb, {"mv", "SAVED", "SAVED/INSIDE"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "inside itself"), "mv into itself: %s", r.output.c_str());
  r = run(usb, {"mv", "SAVED", "RIDES/00009"});
  EMCOOL_CHECK(r.status != 0 && fs::exists(root / "SAVED"), "mv into RIDES refused: %s", r.output.c_str());
  r = run(usb, {"mv", "RIDES", "OLDRIDES"});
  EMCOOL_CHECK(r.status != 0 && fs::exists(root / "RIDES"), "mv RIDES refused: %s", r.output.c_str());

  // --- rm
  r = run(usb, {"rm", "RIDES/00003/PART0001.LOG"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "being recorded") && fs::exists(root / "RIDES/00003/PART0001.LOG"),
               "rm the live ride refused: %s", r.output.c_str());
  r = run(usb, {"rm", "SAVED"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "rm -r") && fs::exists(root / "SAVED"), "rm a folder needs -r: %s",
               r.output.c_str());
  const int rideChangesBefore = card.rideLogChanges;
  r = run(usb, {"rm", "RIDES/00001/*.LOG"});
  EMCOOL_CHECK(r.status == 0 && !fs::exists(root / "RIDES/00001/PART0001.LOG") && !fs::exists(root / "RIDES/00001/PART0002.LOG"),
               "rm pattern: %s", r.output.c_str());
  EMCOOL_CHECK(contains(r.output, "deleted 2 files"), "rm summary: %s", r.output.c_str());
  EMCOOL_CHECK(card.rideLogChanges == rideChangesBefore + 1, "rm in RIDES tells the ride log: %d", card.rideLogChanges);
  r = run(usb, {"rm", "NOMATCH*"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "nothing matches"), "rm no match: %s", r.output.c_str());
  // rm -r RIDES/* takes every old ride and keeps the one being recorded.
  write(root / "RIDES/00002/PART0002.LOG", "more");
  r = run(usb, {"rm", "-r", "RIDES/*"});
  EMCOOL_CHECK(!fs::exists(root / "RIDES/00001") && !fs::exists(root / "RIDES/00002"), "rm -r old rides: %s",
               r.output.c_str());
  EMCOOL_CHECK(fs::exists(root / "RIDES/00003/PART0001.LOG") && fs::exists(root / "RIDES"), "rm -r keeps the live ride: %s",
               r.output.c_str());
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "being recorded"), "and says so: %s", r.output.c_str());
  // A deep tree goes in steps, and every part of it goes.
  for (int a = 0; a < 3; a++) {
    for (int b = 0; b < 3; b++) write(root / "TREE" / std::to_string(a) / std::to_string(b) / "f.txt", "f");
  }
  usb.setMaxOperationsPerStep(2);
  {
    std::string out;
    const char* words[] = {"rm", "-r", "TREE"};
    bool done = usb.begin(3, words, out);
    int steps = 0;
    while (!done && steps < 1000) {
      done = usb.step(out);
      steps++;
    }
    EMCOOL_CHECK(done && steps > 5, "rm -r went in steps: %d", steps);
    EMCOOL_CHECK(!fs::exists(root / "TREE"), "rm -r tree: %s", out.c_str());
  }
  usb.setMaxOperationsPerStep(256);
  // cd into a folder, then remove it: the console moves up.
  fs::create_directories(root / "GONE/DEEP");
  run(usb, {"cd", "GONE/DEEP"});
  r = run(usb, {"rm", "-r", "/GONE"});
  r = run(usb, {"pwd"});
  EMCOOL_CHECK(r.output == "/\n", "cwd removed: %s", r.output.c_str());

  // --- the ride log starting: nothing in RIDES changes until it says what it's recording
  write(root / "RIDES/00002/PART0001.LOG", "old");
  card.facts_.rideLogStarting = true;
  r = run(usb, {"rm", "RIDES/00002/PART0001.LOG"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "starting") && fs::exists(root / "RIDES/00002/PART0001.LOG"),
               "ride log starting: %s", r.output.c_str());
  r = run(usb, {"rm", "NOTES.TXT", "-r"});
  EMCOOL_CHECK(r.status == 0 && !fs::exists(root / "NOTES.TXT"), "outside RIDES is fine meanwhile: %s", r.output.c_str());
  card.facts_.rideLogStarting = false;
  write(root / "NOTES.TXT", "first line\r\nsecond line\n");
  r = run(usb, {"rm", "RIDES/00002/PART0001.LOG"});
  EMCOOL_CHECK(r.status == 0, "and then it can: %s", r.output.c_str());

  // --- vcu-params: service mode's, read-only from here
  r = run(usb, {"rm", "vcu-params/latest.json"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "service mode") && fs::exists(root / "vcu-params/latest.json"),
               "vcu-params rm refused: %s", r.output.c_str());
  r = run(usb, {"cp", "NOTES.TXT", "vcu-params"});
  EMCOOL_CHECK(r.status != 0 && !fs::exists(root / "vcu-params/NOTES.TXT"), "vcu-params cp in refused: %s", r.output.c_str());
  r = run(usb, {"rm", "-r", "/*"});
  EMCOOL_CHECK(fs::exists(root / "vcu-params/latest.json") && fs::exists(root / "RIDES/00003/PART0001.LOG"),
               "rm -r /* keeps vcu-params and the live ride: %s", r.output.c_str());
  r = run(usb, {"type", "vcu-params/latest.json"});
  EMCOOL_CHECK(r.status == 0 && contains(r.output, "\"t\":1"), "vcu-params can be read: %s", r.output.c_str());
  r = run(usb, {"cp", "vcu-params/latest.json", "SNAP.JSON"});
  EMCOOL_CHECK(r.status == 0 && fs::exists(root / "SNAP.JSON"), "and copied out of: %s", r.output.c_str());
  r = run(usb, {"dir", "/*.JSON"});
  EMCOOL_CHECK(contains(r.output, " /*.JSON\n"), "no double slash at the top: %s", r.output.c_str());

  write(root / "NOTES.TXT", "first line\r\nsecond line\n");

  // --- one console at a time
  {
    CardShell wifi(card, gate, "Wi-Fi console", 64 * 1024);
    write(root / "COPYME.BIN", std::string(200000, 'c'));
    usb.setMaxOperationsPerStep(1);
    std::string out;
    const char* words[] = {"cp", "COPYME.BIN", "COPY2.BIN"};
    const bool done = usb.begin(3, words, out);
    EMCOOL_CHECK(!done && usb.running(), "a big copy takes steps");
    r = run(wifi, {"rm", "NOTES.TXT"});
    EMCOOL_CHECK(r.status != 0 && contains(r.output, "USB console is changing the card") && fs::exists(root / "NOTES.TXT"),
                 "second console waits: %s", r.output.c_str());
    r = run(wifi, {"dir"});
    EMCOOL_CHECK(r.status == 0, "but can still list: %s", r.output.c_str());
    // Stopped half-way: the part-made copy goes.
    usb.cancel(out);
    EMCOOL_CHECK(!fs::exists(root / "COPY2.BIN") && !usb.running(), "cancel removes the part-made copy: %s", out.c_str());
    r = run(wifi, {"rm", "COPYME.BIN"});
    EMCOOL_CHECK(r.status == 0, "the gate is free again: %s", r.output.c_str());
    usb.setMaxOperationsPerStep(256);
  }

  // --- the card pulled mid-copy
  {
    write(root / "COPYME.BIN", std::string(200000, 'c'));
    usb.setMaxOperationsPerStep(1);
    std::string out;
    const char* words[] = {"cp", "COPYME.BIN", "COPY3.BIN"};
    bool done = usb.begin(3, words, out);
    done = done || usb.step(out);
    EMCOOL_CHECK(!done, "mid-copy");
    card.mounted = false;
    card.generation++;
    done = usb.step(out);
    EMCOOL_CHECK(done && contains(out, "card went away"), "pulled card: %s", out.c_str());
    card.mounted = true;
    r = run(usb, {"dir"});
    EMCOOL_CHECK(r.status == 0, "and the next command runs on the new mount: %s", r.output.c_str());
    usb.setMaxOperationsPerStep(256);
  }
  card.mounted = false;
  r = run(usb, {"dir"});
  EMCOOL_CHECK(r.status != 0 && contains(r.output, "no card"), "no card: %s", r.output.c_str());
  card.mounted = true;

  // --- output cap: a listing too long for the console says how much it left out
  {
    for (int at = 0; at < 150; at++) write(root / "MANY" / ("F" + std::to_string(at) + ".TXT"), "m");
    CardShell narrow(card, gate, "Bike console", 2000);
    r = run(narrow, {"dir", "MANY"});
    EMCOOL_CHECK(r.output.size() <= 2100 && contains(r.output, "more lines not shown"), "dir capped: %zu %s", r.output.size(),
                 r.output.substr(r.output.size() - 120).c_str());
    r = run(usb, {"dir", "MANY"});
    EMCOOL_CHECK(contains(r.output, "and 50 more"), "dir lists 100 and counts the rest: %s",
                 r.output.substr(r.output.size() - 200).c_str());
  }

  EMCOOL_CHECK(CardShell::handles("dir") && CardShell::handles("rm") && !CardShell::handles("show"), "handles");

  fs::remove_all(root);
  const int failures = emcool::test::failures();
  std::fprintf(stderr, "check_card_shell: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
