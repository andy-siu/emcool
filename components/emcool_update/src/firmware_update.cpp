#include "emcool/update/firmware_update.hpp"

#include <cstring>

#include "emcool/log.hpp"

namespace emcool::update {

namespace {

constexpr const char* kTag = "update";
constexpr std::uint8_t kImageMagic = 0xE9;
constexpr std::uint16_t kChipEsp32 = 0x0000;
constexpr std::uint32_t kAppDescMagic = 0xABCD5432;
// The image header, then the first segment's header; the app description opens that segment.
constexpr std::size_t kImageHeaderBytes = 24;
constexpr std::size_t kSegmentHeaderBytes = 8;
constexpr std::size_t kDescription = kImageHeaderBytes + kSegmentHeaderBytes;

std::uint32_t little32(std::span<const std::uint8_t> bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
         static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// A NUL-padded text field, stopped at its first NUL and kept to printable characters.
std::string field(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t length) {
  std::string text;
  for (std::size_t index = 0; index < length; index++) {
    const std::uint8_t byte = bytes[at + index];
    if (byte == 0) break;
    text += byte >= 0x20 && byte < 0x7f ? static_cast<char>(byte) : '?';
  }
  return text;
}

}  // namespace

HeaderCheck checkHeader(std::span<const std::uint8_t> head) {
  HeaderCheck check;
  if (head.size() < kHeaderBytes) {
    check.problem = "too short to be firmware (" + std::to_string(head.size()) + " bytes)";
    return check;
  }
  if (head[0] != kImageMagic) {
    check.problem = "not an ESP32 firmware image (it should start with 0xE9); use build/emcool.bin";
    return check;
  }
  const std::uint16_t chip = static_cast<std::uint16_t>(head[12] | head[13] << 8);
  if (chip != kChipEsp32) {
    check.problem = "firmware for another chip (id " + std::to_string(chip) + "), not the ESP32";
    return check;
  }
  if (head[1] == 0) {
    check.problem = "a firmware image with no segments";
    return check;
  }
  const std::span<const std::uint8_t> description = head.subspan(kDescription);
  if (little32(description, 0) != kAppDescMagic) {
    // A bootloader or partition table starts 0xE9 too, but carries no app description.
    check.problem = "an ESP32 image, but not an app (no app description): use build/emcool.bin, not the "
                    "bootloader or a merged image";
    return check;
  }
  ImageInfo info;
  info.version = field(description, 16, 32);
  info.project = field(description, 48, 32);
  info.built = field(description, 96, 16) + " " + field(description, 80, 16);
  info.idfVersion = field(description, 112, 32);
  if (info.project != kProject) {
    check.problem = "this is \"" + info.project + "\" firmware, not emcool's";
    return check;
  }
  check.info = std::move(info);
  return check;
}

std::string describe(const ImageInfo& info) {
  return "emcool " + info.version + ", built " + info.built + ", ESP-IDF " + info.idfVersion;
}

Updater::~Updater() { cancel("the updater went away"); }

bool Updater::start(std::size_t bytes, std::string source, std::string& why) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kWriting) {
    why = "an update from " + source_ + " is already running (" + std::to_string(received_ * 100 / (total_ ? total_ : 1)) +
          "% received)";
    return false;
  }
  if (bytes < kHeaderBytes) {
    why = "too short to be firmware (" + std::to_string(bytes) + " bytes)";
    return false;
  }
  if (bytes > slot_.capacity()) {
    why = std::to_string(bytes) + " bytes won't fit the " + std::to_string(slot_.capacity()) + "-byte app slot";
    return false;
  }
  state_ = State::kWriting;
  source_ = std::move(source);
  total_ = bytes;
  received_ = 0;
  begun_ = false;
  head_.clear();
  incoming_.reset();
  problem_.clear();
  return true;
}

bool Updater::feed(std::span<const std::uint8_t> data) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kWriting) return false;
  if (received_ + data.size() > total_) {
    return fail("more than the " + std::to_string(total_) + " bytes announced");
  }
  // Nothing goes to the slot (nothing is erased) until the header says this is emcool firmware.
  if (!begun_) {
    const std::size_t want = kHeaderBytes - head_.size();
    const std::size_t take = std::min(want, data.size());
    head_.append(reinterpret_cast<const char*>(data.data()), take);
    received_ += take;
    data = data.subspan(take);
    if (head_.size() < kHeaderBytes) return true;
    const std::span<const std::uint8_t> head(reinterpret_cast<const std::uint8_t*>(head_.data()), head_.size());
    HeaderCheck check = checkHeader(head);
    if (!check.info) return fail(check.problem);
    incoming_ = check.info;
    std::string why;
    if (!slot_.begin(total_, why)) return fail("the app slot wouldn't start: " + why);
    begun_ = true;
    logInfo(kTag, "writing %s from %s (%zu bytes) into the spare slot; now running %s", describe(*incoming_).c_str(),
            source_.c_str(), total_, describe(slot_.running()).c_str());
    if (!slot_.write(head, why)) return fail("writing the slot: " + why);
    head_.clear();
    head_.shrink_to_fit();
  }
  if (data.empty()) return true;
  std::string why;
  if (!slot_.write(data, why)) return fail("writing the slot: " + why);
  received_ += data.size();
  return true;
}

bool Updater::finish() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kWriting) return false;
  if (received_ != total_) {
    return fail("the image ended after " + std::to_string(received_) + " of " + std::to_string(total_) + " bytes");
  }
  std::string why;
  if (!slot_.finish(why)) {
    begun_ = false;  // finish() has closed the slot either way
    return fail("the image didn't check out, so nothing changed: " + why);
  }
  begun_ = false;
  state_ = State::kDone;
  logWarning(kTag, "%s is written and checked, and boots on trial at the next restart",
             describe(*incoming_).c_str());
  return true;
}

void Updater::cancel(std::string_view why) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kWriting) return;
  fail(std::string(why));
}

Updater::State Updater::state() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

// Under mutex_.
bool Updater::fail(std::string why) {
  if (begun_) slot_.abandon();
  begun_ = false;
  head_.clear();
  state_ = State::kFailed;
  problem_ = std::move(why);
  logWarning(kTag, "update from %s stopped: %s; the running firmware is unchanged", source_.c_str(), problem_.c_str());
  return false;
}

}  // namespace emcool::update
