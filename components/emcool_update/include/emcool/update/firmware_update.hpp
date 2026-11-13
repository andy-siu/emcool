#pragma once

// Over-the-air updates: a new firmware image written into the spare app slot while the running one
// carries on, then booted on trial. Platform-free, so the host checks run it on a real build's
// emcool.bin; the firmware supplies the slot (esp_ota_*).
//
// How a slot is written (ESP-IDF's two-slot OTA, partitions.csv's ota_0 and ota_1):
//   1. The image's first bytes are checked before anything is erased: an ESP32 app image (magic
//      0xE9, chip ESP32), whose app description names this project, "emcool". A file that is
//      something else, or another project's firmware, is refused with the slot untouched.
//   2. The spare slot is erased as it is written, and the image streamed into it.
//   3. At the end, the slot checks the whole image (esp_ota_end: every segment's checksum, and the
//      SHA-256 the build appends) before anything switches. A short or corrupt file is refused there.
//   4. Only then is the spare slot chosen for the next boot. The running image stays in its slot.
//
// The next boot runs the new image on trial (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): it confirms
// itself once startup has settled (main/firmware_update.cpp), and a crash or power cut before then
// boots the old image again. One update at a time, from any console or the web page.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace emcool::update {

// What an image says about itself: the app description the build embeds (esp_app_desc_t).
struct ImageInfo {
  std::string project;
  std::string version;
  std::string idfVersion;
  std::string built;  // "Nov 13 2026 14:37:18"
};

// The ESP32 image's first bytes are enough to read its description: the 24-byte header, the first
// segment's 8-byte header, then esp_app_desc_t's first 128 bytes.
inline constexpr std::size_t kHeaderBytes = 24 + 8 + 128;
inline constexpr std::string_view kProject = "emcool";

// The description, or why `head` (the image's first kHeaderBytes or more) isn't an emcool image.
struct HeaderCheck {
  std::optional<ImageInfo> info;
  std::string problem;
};
HeaderCheck checkHeader(std::span<const std::uint8_t> head);

// The spare slot, as the firmware has it. One write at a time.
class Slot {
 public:
  virtual ~Slot() = default;
  // The slot's size, and the running image's description, for the messages.
  virtual std::size_t capacity() const = 0;
  virtual ImageInfo running() const = 0;
  // Starts writing an image of `bytes` (erasing as it goes); false, with why, if it can't.
  virtual bool begin(std::size_t bytes, std::string& why) = 0;
  virtual bool write(std::span<const std::uint8_t> data, std::string& why) = 0;
  // Checks the whole image and chooses it for the next boot; false, with why, if it isn't sound.
  virtual bool finish(std::string& why) = 0;
  virtual void abandon() = 0;
};

// One update, fed in pieces of any size, from wherever the image comes from.
class Updater {
 public:
  enum class State : std::uint8_t { kIdle, kWriting, kDone, kFailed };

  explicit Updater(Slot& slot) : slot_(slot) {}
  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;
  ~Updater();

  // Starts an update of `bytes`, from `source` ("the card's /emcool.bin", "Wi-Fi"). False, with why,
  // when another is running or the image can't fit.
  bool start(std::size_t bytes, std::string source, std::string& why);
  // The next piece. False once it has failed; problem() says why, and the slot is left as it was.
  bool feed(std::span<const std::uint8_t> data);
  // After the last piece: true once the new image is chosen for the next boot.
  bool finish();
  void cancel(std::string_view why);

  State state() const;
  const std::string& problem() const { return problem_; }
  const std::optional<ImageInfo>& incoming() const { return incoming_; }
  std::size_t received() const { return received_; }
  std::size_t total() const { return total_; }
  const std::string& source() const { return source_; }

 private:
  bool fail(std::string why);

  Slot& slot_;
  mutable std::mutex mutex_;
  State state_ = State::kIdle;
  std::string source_;
  std::size_t total_ = 0;
  std::size_t received_ = 0;
  bool begun_ = false;
  std::string head_;
  std::optional<ImageInfo> incoming_;
  std::string problem_;
};

// "Nov 13 2026 14:37:18, emcool 2bb2ec5, ESP-IDF v6.1"
std::string describe(const ImageInfo& info);

}  // namespace emcool::update
