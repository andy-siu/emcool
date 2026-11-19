#pragma once

// The bike's side of over-the-air updates (emcool/update/firmware_update.hpp): the spare app slot
// through esp_ota_*, the trial boot's confirmation, and the two ways an image arrives:
//
//   `update FILE`   on any console: an image already on the microSD card, `update /emcool.bin`.
//   PUT /update     on the Wi-Fi page, behind ADMIN_PASSWORD (main/wifi_link.cpp):
//                   curl -u :PASSWORD -T build/emcool.bin http://emcool/update
//   /firmware/…     over Bluetooth, from the dashboard's Settings → emcool, a piece per request
//                   (emcool/ble_server/firmware_endpoints.hpp)
//
// Either way the image is checked, written and checked again, and boots at the next `restart`. A
// new image boots on trial: once it has run kTrialMs it is kept; a crash, a watchdog or a power cut
// before then boots the old one again. `update status` says what's running, on trial, or waiting.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emcool/ble_server/firmware_endpoints.hpp"
#include "emcool/files/card_shell.hpp"
#include "emcool/update/firmware_update.hpp"

namespace emcool {

// How long a new image must run before it's kept: past the first status report and card write, as
// boot_guard's settling time is.
inline constexpr std::int64_t kTrialMs = 30 * 1000;

// The one updater, shared by every console and the web page.
update::Updater& firmwareUpdater();

// What stops an update starting: writing flash pauses everything not in IRAM, the CAN bus's interrupt
// among them, for tens of milliseconds at a time, which drops frames and could time out a service
// write halfway. So an update waits for what a restart waits for: a parked bike and a free bus
// (DashboardConsoleHost::restartBlockers). Unset (safe mode, a failed startup): nothing to wait for.
void setUpdateBlockers(std::function<std::vector<std::string>()> blockers);
// Why an update can't start now, or empty.
std::string updateRefusal();

// What runs now, whether it's on trial, what the next boot runs, and any rollback: `update status`'s
// lines, and the dashboard's.
std::string firmwareReport();

// The dashboard's side of an update over Bluetooth: the same blockers, the same report.
class BluetoothFirmwareHost final : public ble_server::FirmwareHost {
 public:
  std::string refusal() override { return updateRefusal(); }
  std::string report() override { return firmwareReport(); }
};

// From app_main, first thing: if this boot is a new image on trial, says so, and keeps it once it
// has run kTrialMs. With rollback off in the bootloader there is no trial, and it says nothing.
void startFirmwareTrial();

// `update FILE`, `update status`: what the console prints, and 0 when it did what was asked. Reads
// the file from `card` (null without a card) a piece at a time, printing progress through `flush`.
int runUpdateCommand(int argumentCount, const char* const* arguments, files::Card* card,
                     const std::function<void(std::string&)>& flush);

// What the help lists.
inline constexpr const char* kUpdateHelp =
    "Install new firmware from the card: `update /emcool.bin` (copy build/emcool.bin onto the card first). It's "
    "checked, written beside the running firmware, and boots at the next restart, on trial: a crash in its first "
    "30 s goes back to the old one, and so does a `restart` in its first 30 s. Refused while the bike is moving "
    "or service mode has the bus. `update status` says what's running and what's waiting.";
inline constexpr const char* kUpdateHint = "FILE | status";

}  // namespace emcool
