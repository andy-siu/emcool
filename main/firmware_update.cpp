#include "firmware_update.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <memory>

#include "emcool/log.hpp"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

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
    out = "no card: copy build/emcool.bin onto the microSD card, or send it over Wi-Fi (PUT /update)\n";
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
  const std::string full = card->mountPoint() + path;

  std::uint32_t generation = 0;
  std::size_t bytes = 0;
  int fd = -1;
  int error = 0;
  {
    const std::lock_guard<std::mutex> lock(card->lock());
    const files::CardState state = card->state();
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
    out = path + ": " + (error == ENODEV ? "the card isn't mounted" : error == ENOENT ? "not found" : std::strerror(error)) +
          "\n";
    flush(out);
    return 1;
  }

  update::Updater& updater = firmwareUpdater();
  std::string why = updateRefusal();
  if (!why.empty()) why = "not now: " + why;
  if (!why.empty() || !updater.start(bytes, "the card's " + path, why)) {
    {
      const std::lock_guard<std::mutex> lock(card->lock());
      if (card->state().generation == generation) ::close(fd);
    }
    out = "not updating: " + why + "\n";
    flush(out);
    return 1;
  }
  out = "updating from " + path + " (" + std::to_string(bytes) + " bytes)\n";
  flush(out);
  const auto buffer = std::make_unique<std::uint8_t[]>(kReadBytes);
  std::size_t shown = 0;
  bool cardGone = false;
  while (updater.state() == update::Updater::State::kWriting && updater.received() < bytes) {
    ssize_t got = 0;
    {
      const std::lock_guard<std::mutex> lock(card->lock());
      const files::CardState state = card->state();
      if (!state.mounted || state.generation != generation) {
        cardGone = true;  // the descriptor went with the card: forgotten, never closed
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
  if (!cardGone) {
    const std::lock_guard<std::mutex> lock(card->lock());
    if (card->state().generation == generation) ::close(fd);
  }
  if (cardGone) updater.cancel("the card went away mid-update");
  const bool done = updater.state() == update::Updater::State::kWriting && updater.finish();
  if (!done) {
    out = "not updated: " + updater.problem() + "\nThe running firmware is unchanged.\n";
    flush(out);
    return 1;
  }
  out = "written and checked: " + update::describe(*updater.incoming()) +
        "\n`restart` to run it. It boots on trial: let it run 30 s, since a crash or a restart before then goes back "
        "to the firmware running now.\n";
  flush(out);
  return 0;
}

}  // namespace emcool
