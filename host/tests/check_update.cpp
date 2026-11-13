// Over-the-air updates (emcool/update/firmware_update.hpp) against a fake slot, fed a real build's
// emcool.bin: what is accepted, and above all that anything else — another chip, another project,
// a bootloader, a file cut short, one that runs long — is refused with the slot never erased, or
// abandoned so the running firmware stays chosen.
//
//   check_update [build/emcool.bin]
//
// With no image (tools/check.sh doesn't build the firmware), it makes one with the same layout: the
// header, a segment header, the app description, then filler.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/update/firmware_update.hpp"

using emcool::update::ImageInfo;
using emcool::update::Updater;

namespace {

class FakeSlot final : public emcool::update::Slot {
 public:
  std::size_t capacity() const override { return 0x1E0000; }
  ImageInfo running() const override { return {"emcool", "old", "v6.1", "Nov 1 2026 10:00:00"}; }
  bool begin(std::size_t bytes, std::string&) override {
    begins++;
    open = true;
    expected = bytes;
    written.clear();
    return true;
  }
  bool write(std::span<const std::uint8_t> data, std::string& why) override {
    if (!open) {
      why = "not open";
      return false;
    }
    if (failWriteAt && written.size() + data.size() > *failWriteAt) {
      why = "flash write failed";
      return false;
    }
    written.insert(written.end(), data.begin(), data.end());
    return true;
  }
  bool finish(std::string& why) override {
    open = false;
    if (corruptAtFinish) {
      why = "image hash mismatch";
      return false;
    }
    chosen = true;
    return true;
  }
  void abandon() override {
    open = false;
    abandons++;
  }

  int begins = 0;
  int abandons = 0;
  bool open = false;
  bool chosen = false;
  bool corruptAtFinish = false;
  std::optional<std::size_t> failWriteAt;
  std::size_t expected = 0;
  std::vector<std::uint8_t> written;
};

std::vector<std::uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Feeds `image` in pieces of `piece` bytes; true if every piece was taken and finish() chose it.
bool feedAll(Updater& updater, const std::vector<std::uint8_t>& image, std::size_t piece) {
  for (std::size_t at = 0; at < image.size(); at += piece) {
    const std::size_t take = std::min(piece, image.size() - at);
    if (!updater.feed(std::span<const std::uint8_t>(image.data() + at, take))) return false;
  }
  return updater.finish();
}

// An image laid out as the build's: 24-byte header, segment header, esp_app_desc_t, filler.
std::vector<std::uint8_t> madeUpImage() {
  std::vector<std::uint8_t> image(300000, 0x5a);
  std::fill(image.begin(), image.begin() + 32 + 256, 0);
  image[0] = 0xE9;  // magic
  image[1] = 6;     // segments
  image[23] = 1;    // SHA-256 appended
  const std::uint32_t descMagic = 0xABCD5432;
  for (int at = 0; at < 4; at++) image[32 + at] = static_cast<std::uint8_t>(descMagic >> (8 * at));
  const auto put = [&](std::size_t at, const char* text) {
    for (std::size_t index = 0; text[index] != 0; index++) image[32 + at + index] = static_cast<std::uint8_t>(text[index]);
  };
  put(16, "test-build");
  put(48, "emcool");
  put(80, "14:37:18");
  put(96, "Nov 13 2026");
  put(112, "v6.1");
  return image;
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount > 2) {
    std::fprintf(stderr, "usage: check_update [build/emcool.bin]\n");
    return 2;
  }
  const std::vector<std::uint8_t> image = argumentCount == 2 ? readFile(arguments[1]) : madeUpImage();
  EMCOOL_CHECK(image.size() > 100000, "the image: %zu bytes", image.size());
  std::fprintf(stderr, "check_update: %s, %zu bytes\n", argumentCount == 2 ? arguments[1] : "a made-up image",
               image.size());
  std::string why;

  // --- the real image: its description, and a whole update in odd-sized pieces
  {
    const emcool::update::HeaderCheck check = emcool::update::checkHeader(image);
    EMCOOL_CHECK(check.info && check.info->project == "emcool" && !check.info->version.empty() &&
                     check.info->idfVersion.starts_with("v"),
                 "header of the real image: %s", check.problem.c_str());
    FakeSlot slot;
    Updater updater(slot);
    EMCOOL_CHECK(updater.start(image.size(), "a test", why), "start: %s", why.c_str());
    // 7 bytes first: the header arrives across pieces.
    EMCOOL_CHECK(updater.feed(std::span<const std::uint8_t>(image.data(), 7)), "first 7 bytes");
    EMCOOL_CHECK(slot.begins == 0, "nothing erased before the header is whole");
    std::vector<std::uint8_t> rest(image.begin() + 7, image.end());
    EMCOOL_CHECK(feedAll(updater, rest, 4093), "the rest: %s", updater.problem().c_str());
    EMCOOL_CHECK(slot.chosen && slot.written == image && updater.state() == Updater::State::kDone,
                 "the slot holds exactly the image (%zu of %zu bytes)", slot.written.size(), image.size());
  }

  // --- refused before the slot is touched
  const auto refused = [&](std::vector<std::uint8_t> bytes, const char* expect, const char* what) {
    FakeSlot slot;
    Updater updater(slot);
    if (!updater.start(bytes.size(), "a test", why)) {
      EMCOOL_CHECK(why.find(expect) != std::string::npos, "%s refused at start: %s", what, why.c_str());
      return;
    }
    const bool taken = feedAll(updater, bytes, 4096);
    EMCOOL_CHECK(!taken && slot.begins == 0 && !slot.chosen, "%s: never erased the slot", what);
    EMCOOL_CHECK(updater.problem().find(expect) != std::string::npos, "%s: %s", what, updater.problem().c_str());
  };
  {
    std::vector<std::uint8_t> bytes = image;
    bytes[0] = 'P';
    refused(bytes, "not an ESP32 firmware image", "a text file");
  }
  {
    std::vector<std::uint8_t> bytes = image;
    bytes[12] = 5;  // ESP32-C3
    refused(bytes, "another chip", "another chip's firmware");
  }
  {
    std::vector<std::uint8_t> bytes = image;
    bytes[32] = 0;  // no app description: a bootloader looks like this
    refused(bytes, "not an app", "a bootloader");
  }
  {
    std::vector<std::uint8_t> bytes = image;
    const char other[] = "garage";
    std::copy(std::begin(other), std::end(other), bytes.begin() + 32 + 48);
    refused(bytes, "\"garage\" firmware", "another project");
  }
  refused(std::vector<std::uint8_t>(image.begin(), image.begin() + 20), "too short", "20 bytes");
  {
    FakeSlot slot;
    Updater updater(slot);
    EMCOOL_CHECK(!updater.start(slot.capacity() + 1, "a test", why) && why.find("won't fit") != std::string::npos,
                 "too big for the slot: %s", why.c_str());
  }

  // --- refused after writing began: the slot is abandoned, never chosen
  {
    FakeSlot slot;
    Updater updater(slot);
    updater.start(image.size(), "a test", why);
    std::vector<std::uint8_t> half(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(image.size() / 2));
    for (std::size_t at = 0; at < half.size(); at += 4096) {
      updater.feed(std::span<const std::uint8_t>(half.data() + at, std::min<std::size_t>(4096, half.size() - at)));
    }
    EMCOOL_CHECK(!updater.finish() && slot.abandons == 1 && !slot.chosen &&
                     updater.problem().find("ended after") != std::string::npos,
                 "cut short: %s", updater.problem().c_str());
  }
  {
    FakeSlot slot;
    Updater updater(slot);
    updater.start(image.size() - 100, "a test", why);
    EMCOOL_CHECK(!feedAll(updater, image, 4096) && slot.abandons == 1 && !slot.chosen &&
                     updater.problem().find("more than") != std::string::npos,
                 "runs long: %s", updater.problem().c_str());
  }
  {
    FakeSlot slot;
    slot.corruptAtFinish = true;
    Updater updater(slot);
    updater.start(image.size(), "a test", why);
    EMCOOL_CHECK(!feedAll(updater, image, 4096) && !slot.chosen &&
                     updater.problem().find("didn't check out") != std::string::npos,
                 "corrupt at the end: %s", updater.problem().c_str());
  }
  {
    FakeSlot slot;
    slot.failWriteAt = 100000;
    Updater updater(slot);
    updater.start(image.size(), "a test", why);
    EMCOOL_CHECK(!feedAll(updater, image, 4096) && slot.abandons == 1 && !slot.chosen,
                 "a flash write fails: %s", updater.problem().c_str());
  }

  // --- one at a time, and cancelling
  {
    FakeSlot slot;
    Updater updater(slot);
    updater.start(image.size(), "the USB console", why);
    updater.feed(std::span<const std::uint8_t>(image.data(), 8192));
    EMCOOL_CHECK(!updater.start(image.size(), "Wi-Fi", why) && why.find("the USB console") != std::string::npos,
                 "a second update waits: %s", why.c_str());
    updater.cancel("stopped by hand");
    EMCOOL_CHECK(slot.abandons == 1 && !slot.chosen && updater.state() == Updater::State::kFailed, "cancel");
    EMCOOL_CHECK(updater.start(image.size(), "Wi-Fi", why), "then another can start: %s", why.c_str());
    EMCOOL_CHECK(feedAll(updater, image, 65536) && slot.chosen, "and finish: %s", updater.problem().c_str());
  }

  std::fprintf(stderr, "check_update: %d failures\n", emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
