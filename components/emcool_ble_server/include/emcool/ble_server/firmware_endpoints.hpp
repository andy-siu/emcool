#pragma once

// Firmware updates over the Bluetooth link: the dashboard's Settings → emcool → Firmware. The image
// goes in pieces, one request each, into the same Updater the consoles' `update`
// use (emcool/update/firmware_update.hpp), so it is checked the same way: the header before anything
// is erased, the whole image's hash before the boot slot changes.
//
//   GET  /firmware                  what runs now, what the next boot runs, and any update under way. JSON.
//   POST /firmware/begin?bytes=N    starts one: refused (409) while the bike is moving or the bus is in
//                                   use, while the card or Wi-Fi is updating, or if N won't fit the slot.
//   POST /firmware/chunk?offset=K   the image's bytes from K, at most kChunkBytes, as the body. K must be
//                                   what the bike has so far: a piece lost or sent twice is refused with
//                                   the offset it expected, never written in the wrong place.
//   POST /firmware/finish           after the last piece: the image is checked, and chosen for the next
//                                   boot. It runs at the next `restart`, on trial.
//   POST /firmware/cancel           stops a Bluetooth update; the running firmware is unchanged.
//
// Every POST needs X-Emcool: firmware. Each answers JSON with `ok` and `message`. The phone going
// away mid-update (sessionEnded()) stops it, as does the bike starting to move.
//
// Only the Bluetooth key stands between a phone and new firmware: the image isn't signed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "emcool/update/firmware_update.hpp"

namespace emcool::ble_server {

struct FirmwareAnswer {
  int status;
  std::string body;  // JSON
};

// What the firmware supplies.
class FirmwareHost {
 public:
  virtual ~FirmwareHost() = default;
  // Why an update can't start or carry on now (the bike is moving, the bus is busy), or empty.
  virtual std::string refusal() = 0;
  // `update status`'s lines: what runs, whether it's on trial, what the next boot runs.
  virtual std::string report() = 0;
};

class FirmwareEndpoints {
 public:
  static constexpr std::string_view kHeaderValue = "firmware";
  // How the Updater names this source, in `update status` and the log.
  static constexpr std::string_view kSource = "Bluetooth";
  // The most of the image one /firmware/chunk carries: with its header, inside one message.
  static constexpr std::size_t kChunkBytes = 2048;

  FirmwareEndpoints(update::Updater& updater, FirmwareHost& host) : updater_(updater), host_(host) {}

  static bool handles(std::string_view route) { return route == "/firmware" || route.starts_with("/firmware/"); }
  // A piece of the image, whose body may be up to kChunkBytes.
  static bool isChunk(std::string_view path) { return path.starts_with("/firmware/chunk"); }

  FirmwareAnswer answer(std::string_view method, std::string_view route, std::string_view query,
                        std::string_view header, std::string_view body);
  // The phone went away, or another session began: a Bluetooth update under way stops.
  void sessionEnded();

 private:
  FirmwareAnswer status();
  FirmwareAnswer begin(std::string_view query);
  FirmwareAnswer chunk(std::string_view query, std::string_view body);
  FirmwareAnswer finish();
  FirmwareAnswer cancel();
  bool ours() const;

  update::Updater& updater_;
  FirmwareHost& host_;
  std::size_t loggedTenth_ = 0;
};

}  // namespace emcool::ble_server
