#pragma once

// The dashboard's Settings → Bike console: the serial console's setup and status, over the
// Bluetooth link. The same commands, the same checks and the same NVS, so a change made here is a
// change made at `emcool>`, and `show` there says so.
//
//   GET  /console          every setting with its saved value, the value in force, whether it is
//                          waiting for a restart, and its help, plus the revision. JSON.
//   GET  /console/status   the status report the serial port prints, plus the `network` report.
//                          Text, as the console would print it.
//   POST /console          one command line, as typed at `emcool>`, in a JSON body:
//                          {"line": "set OBD_ENABLED 1", "revision": 7}, behind X-Emcool: console.
//                          Answers {ok, output, revision, restarting}.
//
// The commands: show, set NAME VALUE, unset NAME, status [on | off | every SECONDS], network,
// restart and help, and the card's (emcool/files/card_shell.hpp): dir, cd, pwd, type, mkdir, rmdir,
// rm, cp and mv. A card command that takes a while (rm -r, a big cp) runs a slice per poll(), so it
// never holds up the link; its output comes back when it has finished, capped at kMaxOutputBytes. `revision` counts what has been saved since boot, from any console. A command
// that saves is refused once another console or phone has saved since the page read it, so a
// stale page can't overwrite a change it never saw. Reads and `restart` need no revision.
//
// `set BLE_SECRET <key>` works here as on the console; the Bike console's New key makes one on the
// phone. Bluetooth is already on whenever this page can reach it, so a new key waits for the
// restart. Secrets are never sent back: a password reads as set or not, with its length, as `show`
// prints it.
//
// `restart` waits for a parked bike and a free bus: it is refused while a fresh reading says the
// bike is moving, in drive or asked to be, or while a service read or write holds the bus. And, as
// the reference's Bluetooth restart is, while a new BLE_SECRET is saved but the request doesn't
// carry it as {"bluetoothKey": …}: a phone that restarts into a key it doesn't hold is locked out
// until someone reaches the USB console. The restart itself happens after the reply has gone.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "emcool/ble/json_reader.hpp"
#include "emcool/files/card_shell.hpp"
#include "emcool/settings/settings.hpp"

namespace emcool::ble_server {

struct ConsoleAnswer {
  int status;
  const char* contentType;
  std::string body;
};

// What the firmware supplies: the reports, what stops a restart, and the restart.
class ConsoleHost {
 public:
  virtual ~ConsoleHost() = default;
  // The status report's text, once the task that owns its counters has written it: `first` asks
  // for it, and the calls after look for it. False until it is there.
  virtual bool statusReport(bool first, std::string& text) = 0;
  // The `network` command's report.
  virtual std::string network() = 0;
  // Why a restart can't happen now (the bike is moving, the bus is busy, …); empty when it can.
  virtual std::vector<std::string> restartBlockers() = 0;
  // Restarts in a moment, once the reply has had time to reach the phone.
  virtual void restartSoon() = 0;
};

class ConsoleEndpoints {
 public:
  static constexpr std::string_view kHeaderValue = "console";
  // As the Wi-Fi console's: a command needs far less, a secret 128 characters.
  static constexpr std::size_t kMaxLineBytes = 512;
  // What a card command prints, at most: a reply is one message the phone holds whole.
  static constexpr std::size_t kMaxOutputBytes = 6 * 1024;

  // `card`, when the bike has one, runs the card's commands; without it they say there's no card.
  ConsoleEndpoints(settings::SettingsStorage& storage, settings::RuntimeSettings& running, ConsoleHost& host,
                   files::CardShell* card = nullptr)
      : storage_(storage), running_(running), host_(host), card_(card) {}

  static bool handles(std::string_view route) { return route == "/console" || route == "/console/status"; }

  // The answer now, or nullopt for one that comes from poll() (the status report).
  std::optional<ConsoleAnswer> begin(std::string_view method, std::string_view route, std::string_view header,
                                     std::string_view body);
  std::optional<ConsoleAnswer> poll();
  // The phone gave up, or went: a card command still running is stopped, and its part-made copy removed.
  void abandon();

 private:
  ConsoleAnswer settingsList();
  std::optional<ConsoleAnswer> run(std::string_view body);
  std::optional<std::string> restartRefusal(const ble::JsonObjectReader& request);

  settings::SettingsStorage& storage_;
  settings::RuntimeSettings& running_;
  ConsoleHost& host_;
  files::CardShell* card_;
  // A card command running across polls, and what it has printed so far.
  bool cardRunning_ = false;
  std::string cardOutput_;
  bool waitingForStatus_ = false;
  bool askedForStatus_ = false;
  // The report answers a typed `status`, as the command's JSON, rather than GET /console/status.
  bool statusAsCommand_ = false;
};

}  // namespace emcool::ble_server
