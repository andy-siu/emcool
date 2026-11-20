#include "firmware_update.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <optional>

#include "emcool/log.hpp"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace emcool {

namespace {

constexpr const char* kTag = "update";
constexpr std::size_t kReadBytes = 4096;

update::ImageInfo infoOf(const esp_app_desc_t& description) {
  return {description.project_name, description.version, description.idf_ver,
          std::string(description.date) + " " + description.time};
}

// The spare slot: whichever of ota_0 and ota_1 isn't running.
class OtaSlot final : public update::Slot {
 public:
  std::size_t capacity() const override {
    const esp_partition_t* spare = esp_ota_get_next_update_partition(nullptr);
    return spare != nullptr ? spare->size : 0;
  }
  update::ImageInfo running() const override { return infoOf(*esp_app_get_description()); }
  std::uint16_t chip() const override { return CONFIG_IDF_FIRMWARE_CHIP_ID; }

  bool begin(std::size_t bytes, std::string& why) override {
    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (partition_ == nullptr) {
      why = "this partition table has no spare app slot";
      return false;
    }
    // Erases as it goes, so the first write doesn't wait for 1.9 MB of erasing.
    const esp_err_t result = esp_ota_begin(partition_, OTA_WITH_SEQUENTIAL_WRITES, &handle_);
    if (result != ESP_OK) {
      why = esp_err_to_name(result);
      return false;
    }
    logInfo(kTag, "writing %zu bytes into %s at 0x%" PRIx32, bytes, partition_->label, partition_->address);
    return true;
  }

  bool write(std::span<const std::uint8_t> data, std::string& why) override {
    const esp_err_t result = esp_ota_write(handle_, data.data(), data.size());
    if (result != ESP_OK) {
      why = esp_err_to_name(result);
      return false;
    }
    return true;
  }

  bool finish(std::string& why) override {
    // Checks every segment and the appended SHA-256 before anything is switched.
    esp_err_t result = esp_ota_end(handle_);
    handle_ = 0;
    if (result != ESP_OK) {
      why = result == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is corrupt (its checksum or hash doesn't match)"
                                                  : esp_err_to_name(result);
      return false;
    }
    result = esp_ota_set_boot_partition(partition_);
    if (result != ESP_OK) {
      why = std::string("couldn't choose it for the next boot: ") + esp_err_to_name(result);
      return false;
    }
    return true;
  }

  void abandon() override {
    if (handle_ != 0) esp_ota_abort(handle_);
    handle_ = 0;
  }

 private:
  const esp_partition_t* partition_ = nullptr;
  esp_ota_handle_t handle_ = 0;
};

std::function<std::vector<std::string>()>& blockers() {
  static std::function<std::vector<std::string>()> installed;
  return installed;
}

OtaSlot& otaSlot() {
  static OtaSlot slot;
  return slot;
}

std::string percent(std::size_t part, std::size_t whole) {
  return std::to_string(whole == 0 ? 0 : part * 100 / whole) + "%";
}

// Which image the next boot runs, and in what state; for `update status`.
std::string slotReport() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  std::string out = "running: " + update::describe(infoOf(*esp_app_get_description())) + " (" +
                    (running != nullptr ? running->label : "?") + ")";
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    out += ", on trial: kept once it has run 30 s (a restart before then goes back)";
  }
  out += "\n";
  if (boot != nullptr && running != nullptr && boot != running) {
    esp_app_desc_t waiting = {};
    if (esp_ota_get_partition_description(boot, &waiting) == ESP_OK) {
      out += "next boot: " + update::describe(infoOf(waiting)) + " (" + boot->label + "), on trial; `restart` to run it\n";
    }
  }
  if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
    esp_app_desc_t rejected = {};
    if (esp_ota_get_partition_description(bad, &rejected) == ESP_OK) {
      out += "rolled back from: " + update::describe(infoOf(rejected)) + " (" + bad->label +
             "): it didn't run its first 30 s\n";
    }
  }
  return out;
}

int status(const std::function<void(std::string&)>& flush) {
  std::string out = slotReport();
  const update::Updater& updater = firmwareUpdater();
  if (updater.state() == update::Updater::State::kWriting) {
    out += "receiving: " + percent(updater.received(), updater.total()) + " of " + std::to_string(updater.total()) +
           " bytes from " + updater.source() + "\n";
  } else if (updater.state() == update::Updater::State::kFailed) {
    out += "last update, from " + updater.source() + ", stopped: " + updater.problem() + "\n";
  }
  flush(out);
  return 0;
}

}  // namespace

std::string firmwareReport() { return slotReport(); }

void setUpdateBlockers(std::function<std::vector<std::string>()> function) { blockers() = std::move(function); }

std::string updateRefusal() {
  if (!blockers()) return {};
  std::string why;
  for (const std::string& blocker : blockers()()) why += (why.empty() ? "" : "; ") + blocker;
  return why;
}

update::Updater& firmwareUpdater() {
  static update::Updater updater(otaSlot());
  return updater;
}

void startFirmwareTrial() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
      logWarning(kTag, "the update in %s was rolled back: it didn't run its first 30 s", bad->label);
    }
    return;
  }
  logWarning(kTag, "%s is new firmware, on trial: it's kept once it has run 30 s. Don't restart before then: a "
                   "crash or a restart in its first 30 s goes back to the old firmware",
             update::describe(infoOf(*esp_app_get_description())).c_str());
  const esp_timer_create_args_t confirm = {
      .callback =
          [](void*) {
            const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
            if (result == ESP_OK) {
              logInfo(kTag, "the new firmware ran 30 s: it's kept");
            } else {
              logError(kTag, "couldn't keep the new firmware (%s); the next restart goes back", esp_err_to_name(result));
            }
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "update-trial",
      .skip_unhandled_events = true,
  };
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&confirm, &timer) != ESP_OK ||
      esp_timer_start_once(timer, static_cast<std::uint64_t>(kTrialMs) * 1000) != ESP_OK) {
    // Without the timer it would never be kept, and every restart would undo it: keep it now.
    logWarning(kTag, "no timer for the trial; keeping the new firmware now");
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

CardInstall installFromCard(files::Card& card, const std::string& path, const std::function<void(std::string&)>& flush,
                            std::string& problem) {
  const std::string full = card.mountPoint() + path;
  std::uint32_t generation = 0;
  std::size_t bytes = 0;
  int fd = -1;
  int error = 0;
  {
    const std::lock_guard<std::mutex> lock(card.lock());
    const files::CardState state = card.state();
    if (!state.mounted) {
      error = ENODEV;
    } else {
      generation = state.generation;
      struct stat info = {};
      if (::stat(full.c_str(), &info) != 0) {
        error = errno;
      } else if (S_ISDIR(info.st_mode)) {
        error = EISDIR;
      } else {
        bytes = static_cast<std::size_t>(info.st_size);
        fd = ::open(full.c_str(), O_RDONLY);
        if (fd < 0) error = errno;
      }
    }
  }
  if (error != 0) {
    problem = path + ": " +
              (error == ENODEV ? "the card isn't mounted" : error == ENOENT ? "not found" : std::strerror(error));
    return error == ENODEV || error == ENOENT ? CardInstall::kNoFile : CardInstall::kUnreadable;
  }
  bool cardGone = false;
  const auto closeFile = [&] {
    if (cardGone) return;  // the descriptor went with the card: forgotten, never closed
    const std::lock_guard<std::mutex> lock(card.lock());
    if (card.state().generation == generation) ::close(fd);
  };

  update::Updater& updater = firmwareUpdater();
  if (const std::string refusal = updateRefusal(); !refusal.empty()) {
    closeFile();
    problem = "not now: " + refusal;
    return CardInstall::kNotNow;
  }
  if (updater.state() == update::Updater::State::kWriting) {
    closeFile();
    problem = "an update from " + updater.source() + " is already running";
    return CardInstall::kNotNow;
  }
  if (!updater.start(bytes, "the card's " + path, problem)) {
    closeFile();
    return CardInstall::kNotStarted;
  }
  std::string out = "updating from " + path + " (" + std::to_string(bytes) + " bytes)\n";
  flush(out);
  const auto buffer = std::make_unique<std::uint8_t[]>(kReadBytes);
  std::size_t shown = 0;
  while (updater.state() == update::Updater::State::kWriting && updater.received() < bytes) {
    ssize_t got = 0;
    {
      const std::lock_guard<std::mutex> lock(card.lock());
      const files::CardState state = card.state();
      if (!state.mounted || state.generation != generation) {
        cardGone = true;
        break;
      }
      got = ::read(fd, buffer.get(), kReadBytes);
    }
    if (got <= 0) break;
    // Written outside the card's lock: a flash write can take a while, and the ride log needs the card.
    if (!updater.feed(std::span<const std::uint8_t>(buffer.get(), static_cast<std::size_t>(got)))) break;
    const std::size_t tenth = updater.received() * 10 / bytes;
    if (tenth > shown) {
      shown = tenth;
      out = "  " + std::to_string(tenth * 10) + "%\n";
      flush(out);
    }
  }
  closeFile();
  if (cardGone) updater.cancel("the card went away mid-update");
  if (updater.state() != update::Updater::State::kWriting || !updater.finish()) {
    problem = updater.problem();
    return CardInstall::kFailed;
  }
  return CardInstall::kInstalled;
}

int runUpdateCommand(int argumentCount, const char* const* arguments, files::Card* card,
                     const std::function<void(std::string&)>& flush) {
  std::string out;
  if (argumentCount == 2 && std::strcmp(arguments[1], "status") == 0) return status(flush);
  if (argumentCount != 2) {
    out = "usage: update FILE (an emcool.bin on the card), or update status\n";
    flush(out);
    return 1;
  }
  if (card == nullptr) {
    out = "no card: the ride log isn't running, so there's no card to install from; use the dashboard's Firmware panel over Bluetooth, or the cable\n";
    flush(out);
    return 1;
  }
  // The path as the card commands name it, from the card's top folder.
  std::string path = arguments[1];
  if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
    out = "give the file's path from the card's top folder, like /emcool.bin\n";
    flush(out);
    return 1;
  }
  if (path.empty() || path.front() != '/') path = "/" + path;

  std::string problem;
  switch (installFromCard(*card, path, flush, problem)) {
    case CardInstall::kInstalled:
      break;
    case CardInstall::kNoFile:
    case CardInstall::kUnreadable:
      out = problem + "\n";
      flush(out);
      return 1;
    case CardInstall::kNotNow:
    case CardInstall::kNotStarted:
      out = "not updating: " + problem + "\n";
      flush(out);
      return 1;
    case CardInstall::kFailed:
      out = "not updated: " + problem + "\nThe running firmware is unchanged.\n";
      flush(out);
      return 1;
  }
  out = "written and checked: " + update::describe(*firmwareUpdater().incoming()) +
        "\n`restart` to run it. It boots on trial: let it run 30 s, since a crash or a restart before then goes back "
        "to the firmware running now.\n";
  flush(out);
  return 0;
}

namespace {

constexpr std::int64_t kCardWatchMs = 10 * 1000;
constexpr std::uint32_t kCardWatchStackBytes = 6144;
constexpr UBaseType_t kCardWatchPriority = 2;
constexpr const char* kCardWatchConsole = "card's firmware update";

struct CardWatch {
  files::Card& card;
  files::CardShellGate& gate;
};

std::int64_t uptimeMs() { return esp_timer_get_time() / 1000; }

bool runningOnTrial() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

// The image's app description, where the build puts it: after the 24-byte image header and the
// first segment's 8-byte header.
constexpr std::size_t kDescriptionAt = 24 + 8;

// What the card's file is to this board, from its app description's ELF hash.
enum class Known : std::uint8_t { kNew, kRunning, kWaiting, kRolledBack, kUnreadable };

bool sameBuild(const esp_app_desc_t& file, const esp_partition_t* partition) {
  esp_app_desc_t other = {};
  return partition != nullptr && esp_ota_get_partition_description(partition, &other) == ESP_OK &&
         std::memcmp(file.app_elf_sha256, other.app_elf_sha256, sizeof(file.app_elf_sha256)) == 0;
}

// Under the card's lock, on the mount `generation`. Missing or short: kNew, for installFromCard to say why.
Known whatIs(files::Card& card, std::uint32_t generation) {
  esp_app_desc_t file = {};
  {
    const std::lock_guard<std::mutex> lock(card.lock());
    const files::CardState state = card.state();
    if (!state.mounted || state.generation != generation) return Known::kUnreadable;
    const int fd = ::open((card.mountPoint() + kCardUpdateFile).c_str(), O_RDONLY);
    if (fd < 0) return Known::kNew;
    const bool read = ::lseek(fd, kDescriptionAt, SEEK_SET) == static_cast<off_t>(kDescriptionAt) &&
                      ::read(fd, &file, sizeof(file)) == static_cast<ssize_t>(sizeof(file));
    ::close(fd);
    if (!read || file.magic_word != ESP_APP_DESC_MAGIC_WORD) return Known::kNew;
  }
  const esp_app_desc_t* running = esp_app_get_description();
  if (std::memcmp(file.app_elf_sha256, running->app_elf_sha256, sizeof(file.app_elf_sha256)) == 0) {
    return Known::kRunning;
  }
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  if (boot != nullptr && boot != esp_ota_get_running_partition() && sameBuild(file, boot)) return Known::kWaiting;
  if (sameBuild(file, esp_ota_get_last_invalid_partition())) return Known::kRolledBack;
  return Known::kNew;
}

// Removes the card's /emcool.bin once it's installed, so the next boot doesn't install it again: on
// the card it was read from, under the lock. Empty once removed, else why not.
std::string removeInstalledFile(files::Card& card, std::uint32_t generation) {
  const std::lock_guard<std::mutex> lock(card.lock());
  const files::CardState state = card.state();
  if (!state.mounted || state.generation != generation) return "the card went away";
  if (::unlink((card.mountPoint() + kCardUpdateFile).c_str()) != 0) return std::strerror(errno);
  return {};
}

void cardWatchTask(void* argument) {
  CardWatch& watch = *static_cast<CardWatch*>(argument);
  // The mount whose /emcool.bin is left alone (refused, rolled back, or the running firmware that
  // couldn't be removed): a card put back in is a new mount, and is looked at again.
  std::optional<std::uint32_t> settled;
  std::string lastWait;
  bool restarting = false;
  const auto waiting = [&](const std::string& why, const char* format) {
    if (why != lastWait) logInfo(kTag, format, kCardUpdateFile, why.c_str());
    lastWait = why;
  };

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(kCardWatchMs));

    if (restarting) {
      const std::string refusal = updateRefusal();
      if (!refusal.empty()) {
        waiting(refusal, "the restart into %s's firmware waits: %s");
        continue;
      }
      logWarning(kTag, "restarting into the firmware from the card");
      vTaskDelay(pdMS_TO_TICKS(500));  // for the log line to go out
      esp_restart();
    }

    // A look, without the consoles' gate, so a console's command is never refused for it.
    files::CardState state;
    bool present = false;
    {
      const std::lock_guard<std::mutex> lock(watch.card.lock());
      state = watch.card.state();
      struct stat info = {};
      present = state.mounted && ::stat((watch.card.mountPoint() + kCardUpdateFile).c_str(), &info) == 0;
    }
    if (!present || settled == state.generation) continue;
    // Past startup, so the readings that say the bike is moving have arrived; and not while this
    // image is on trial, since the spare slot can't be written until it's kept.
    if (uptimeMs() < kTrialMs || runningOnTrial()) continue;

    // The consoles' gate for the whole install: no console copies over the file, moves it or
    // removes it while it's read (FatFs here keeps no record of open files), and one copying it
    // onto the card finishes first.
    std::string holder;
    if (!watch.gate.take(kCardWatchConsole, holder)) {
      waiting("the " + holder + " is changing the card", "%s may be on the card; looking waits: %s");
      continue;
    }
    // Already installed (`update /emcool.bin` on a console leaves the file; so does a removal that
    // failed): removed, and nothing written. Waiting for the next boot: left for the restart. Rolled
    // back once already: left alone, or it would install, fail its trial and roll back on every boot.
    switch (whatIs(watch.card, state.generation)) {
      case Known::kNew:
        break;
      case Known::kRunning: {
        const std::string removal = removeInstalledFile(watch.card, state.generation);
        watch.gate.give(kCardWatchConsole);
        if (removal.empty()) {
          watch.card.changed(false);
          logInfo(kTag, "%s on the card is the firmware running now; removed it", kCardUpdateFile);
        } else {
          logWarning(kTag, "%s on the card is the firmware running now, but couldn't be removed: %s", kCardUpdateFile,
                     removal.c_str());
          settled = state.generation;
        }
        continue;
      }
      case Known::kWaiting:
        watch.gate.give(kCardWatchConsole);
        waiting("it's already written, and runs at the next `restart`", "%s is on the card: %s");
        continue;
      case Known::kRolledBack:
        watch.gate.give(kCardWatchConsole);
        logWarning(kTag, "%s on the card is the firmware that was rolled back (it didn't run its first 30 s); not "
                         "installing it again. Remove it, or put a new build there",
                   kCardUpdateFile);
        settled = state.generation;
        continue;
      case Known::kUnreadable:
        watch.gate.give(kCardWatchConsole);
        continue;
    }

    std::string problem;
    const CardInstall result = installFromCard(
        watch.card, kCardUpdateFile,
        [](std::string& piece) {
          while (!piece.empty() && piece.back() == '\n') piece.pop_back();
          if (!piece.empty()) logInfo(kTag, "%s", piece.c_str());
          piece.clear();
        },
        problem);
    std::string removal;
    if (result == CardInstall::kInstalled) removal = removeInstalledFile(watch.card, state.generation);
    watch.gate.give(kCardWatchConsole);

    switch (result) {
      case CardInstall::kNoFile:  // gone since the look
        break;
      case CardInstall::kNotNow:
        waiting(problem, "%s is on the card; installing it waits: %s");
        break;
      case CardInstall::kUnreadable:
      case CardInstall::kNotStarted:
      case CardInstall::kFailed:
        // Left on the card for the laptop to look at; tried again when the card is next mounted.
        logWarning(kTag, "%s on the card isn't installed: %s. The running firmware is unchanged, and the file stays on "
                         "the card",
                   kCardUpdateFile, problem.c_str());
        settled = state.generation;
        break;
      case CardInstall::kInstalled:
        watch.card.changed(false);
        lastWait.clear();
        // A file left behind isn't installed again: once the new firmware runs, it's the one running.
        if (removal.empty()) {
          logWarning(kTag, "installed %s from the card and removed it: %s. Restarting into it once the bike is parked",
                     kCardUpdateFile, update::describe(*firmwareUpdater().incoming()).c_str());
        } else {
          logWarning(kTag, "installed %s from the card: %s, but couldn't remove it (%s). Restarting into it once the "
                           "bike is parked",
                     kCardUpdateFile, update::describe(*firmwareUpdater().incoming()).c_str(), removal.c_str());
        }
        restarting = true;
        break;
    }
  }
}

}  // namespace

void startCardUpdateWatch(files::Card& card, files::CardShellGate& gate) {
  static CardWatch watch{card, gate};
  if (xTaskCreatePinnedToCore(cardWatchTask, "card-update", kCardWatchStackBytes, &watch, kCardWatchPriority, nullptr,
                              0) != pdPASS) {
    logError(kTag, "could not create the card's firmware-update task; `update %s` still works", kCardUpdateFile);
  }
}

}  // namespace emcool
