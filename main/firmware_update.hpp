#pragma once

// The bike's side of over-the-air updates (emcool/update/firmware_update.hpp): the spare app slot
// through esp_ota_*, the trial boot's confirmation, and the two ways an image arrives:
//
//   /emcool.bin     on the microSD card's top folder: installed on its own, removed, and restarted
//                   into (startCardUpdateWatch)
//   `update FILE`   on the USB and SSH consoles (never the read-only telnet console): an image
//                   already on the microSD card, `update /emcool.bin`, or `ssh admin@emcool update
//                   /emcool.bin` from the laptop.
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

// How an install from the card went.
enum class CardInstall : std::uint8_t {
  kInstalled,   // written, checked, and chosen for the next boot
  kNoFile,      // no such file, or no card mounted
  kUnreadable,  // there, but a folder or a file that won't open
  kNotNow,      // the bike is moving, the bus is busy, another update is running, or the card went away
  kNotStarted,  // the updater refused it before reading: too short, or too big for the slot
  kFailed,      // refused while it was read (another chip, another project, corrupt): the slot unchanged
};

// Installs `path` (from the card's top folder, "/emcool.bin") from `card`: read a piece at a time
// under the card's lock, written outside it, with progress through `flush`. `problem` says why, for
// anything but kInstalled.
CardInstall installFromCard(files::Card& card, const std::string& path, const std::function<void(std::string&)>& flush,
                            std::string& problem);

// The file on the card that installs itself.
inline constexpr const char* kCardUpdateFile = "/emcool.bin";

// Watches the card for /emcool.bin, every 10 s once startup has settled and the running image is
// kept (a trial image can't write the spare slot). When one is there it's installed as `update
// /emcool.bin` would, holding `gate` so no console changes the card meanwhile; then it's removed,
// and the bike restarts into it as soon as nothing blocks a restart (a parked bike, a free bus).
// Before installing, its app description's ELF hash is compared: the firmware running now is just
// removed (a removal that failed, a console's `update` that left it); the one already waiting for
// the next boot is left for a `restart`; and the one that was rolled back is left alone, so a bad
// build doesn't install, fail its trial and roll back on every boot. One that's refused (another
// chip's build, a corrupt copy) stays on the card, logged, until a card is next mounted.
void startCardUpdateWatch(files::Card& card, files::CardShellGate& gate);

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
