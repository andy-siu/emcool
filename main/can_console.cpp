#include "can_console.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "can_bus.hpp"
#include "emcool/can/console.hpp"
#include "emcool/can/psu.hpp"
#include "emcool/log.hpp"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace emcool {

namespace {

constexpr const char* kTag = "can-console";

DiagnosticsTask* consoleDiagnosticsTask = nullptr;

const char* kNoBus = "the CAN bus isn't running (the boot log says why)\n";
const char* kNoPoller = "OBD is off (set OBD_ENABLED 1 and restart); there is no poller\n";

// A repeating can-send gives up after this long, as the reference's PSU hold does, so a console
// left unattended doesn't keep a frame on the bus for ever.
constexpr std::int64_t kMaxRepeatUs = 60ll * 60 * 1000 * 1000;
// Refusals in a row (every transmit slot still waiting, or bus-off) that end a repeating send.
constexpr std::uint32_t kMaxRefusedInARow = 3;

// Puts stdin in non-blocking mode for as long as it lives, so a loop can poll for a keypress.
// What is already waiting when it starts is thrown away: Enter sends CR LF, and the console ends
// the command line at the CR, so the LF behind it would otherwise stop the command at once.
class KeyWatch {
 public:
  KeyWatch() : fd_(fileno(stdin)), savedFlags_(fcntl(fd_, F_GETFL, 0)) {
    if (savedFlags_ != -1) fcntl(fd_, F_SETFL, savedFlags_ | O_NONBLOCK);
    // The LF can trail the CR by a character time or a USB packet; give it a moment to land.
    vTaskDelay(pdMS_TO_TICKS(30));
    pressed();
  }
  ~KeyWatch() {
    if (savedFlags_ != -1) fcntl(fd_, F_SETFL, savedFlags_);
  }
  KeyWatch(const KeyWatch&) = delete;
  KeyWatch& operator=(const KeyWatch&) = delete;

  // True if anything was typed since the last call; swallows it.
  bool pressed() {
    char typed = 0;
    bool any = false;
    while (read(fd_, &typed, 1) > 0) any = true;
    return any;
  }

 private:
  int fd_;
  int savedFlags_;
};

int sendRepeatedly(CanBus& bus, const can::CanSendCommand& command, std::span<const std::uint8_t> payload,
                   const std::string& shown) {
  std::printf("sending %s every %u ms — press Enter to stop\n", shown.c_str(), static_cast<unsigned>(command.repeatMs));
  std::fflush(stdout);

  const CanBusCounters& counters = bus.counters();
  const std::uint32_t sentBefore = counters.sent.load(std::memory_order_relaxed);
  const std::uint32_t failedBefore = counters.sendFailed.load(std::memory_order_relaxed);
  const std::int64_t startedUs = esp_timer_get_time();
  const TickType_t period = std::max<TickType_t>(1, pdMS_TO_TICKS(command.repeatMs));

  // Tee the receive path while sending, so what the bus says back (a node waking up, say) shows in
  // the progress lines: a count and the latest frame per id.
  struct Heard {
    std::uint32_t id;
    bool extended;
    std::uint32_t count;
    ReceivedFrame latest;
  };
  std::vector<Heard> heard;
  const bool listening = bus.startDump();
  const auto drainHeard = [&] {
    ReceivedFrame frame;
    while (listening && bus.nextDumpFrame(frame, 0)) {
      auto found = std::find_if(heard.begin(), heard.end(),
                                [&](const Heard& h) { return h.id == frame.id && h.extended == frame.extended; });
      if (found != heard.end()) {
        found->count++;
        found->latest = frame;
      } else if (heard.size() < 16) {
        heard.push_back({frame.id, frame.extended, 1, frame});
      }
    }
  };
  const auto printHeard = [&](const char* indent) {
    if (heard.empty()) {
      std::printf("%sheard nothing back\n", indent);
      return;
    }
    for (const Heard& h : heard) {
      const std::string line = can::formatCanFrame(
          h.latest.id, std::span<const std::uint8_t>(h.latest.data, h.latest.length), h.latest.extended);
      std::printf("%sheard x%u, latest %s\n", indent, static_cast<unsigned>(h.count), line.c_str());
    }
  };

  std::uint32_t queued = 0;
  std::uint32_t refused = 0;
  std::uint32_t refusedInARow = 0;
  std::string why = "stopped";
  std::optional<std::string> lastProblem;
  KeyWatch keys;
  TickType_t wake = xTaskGetTickCount();
  std::int64_t nextProgressUs = startedUs + 5ll * 1000 * 1000;
  while (true) {
    if (const std::optional<std::string> problem = bus.sendRaw(command.frame.id, payload, command.frame.extended)) {
      refused++;
      lastProblem = problem;
      if (++refusedInARow >= kMaxRefusedInARow) {
        why = "gave up after " + std::to_string(kMaxRefusedInARow) + " refusals in a row (" + *problem + ")";
        break;
      }
    } else {
      queued++;
      refusedInARow = 0;
    }
    drainHeard();
    const std::int64_t nowUs = esp_timer_get_time();
    if (nowUs - startedUs >= kMaxRepeatUs) {
      why = "stopped after 60 minutes";
      break;
    }
    if (nowUs >= nextProgressUs) {
      nextProgressUs += 5ll * 1000 * 1000;
      std::printf("  %u queued, %u acknowledged so far\n", static_cast<unsigned>(queued),
                  static_cast<unsigned>(counters.sent.load(std::memory_order_relaxed) - sentBefore));
      printHeard("    ");
      std::fflush(stdout);
    }
    if (keys.pressed()) break;
    vTaskDelayUntil(&wake, period);
  }

  drainHeard();
  if (listening) bus.stopDump();

  // Acknowledged/failed count every frame the bus sent meanwhile, the poller's too if it is on.
  const unsigned acknowledged = counters.sent.load(std::memory_order_relaxed) - sentBefore;
  const unsigned failed = counters.sendFailed.load(std::memory_order_relaxed) - failedBefore;
  std::printf("%s: %u queued, %u refused, %u acknowledged, %u failed%s\n", why.c_str(), static_cast<unsigned>(queued),
              static_cast<unsigned>(refused), acknowledged, failed,
              acknowledged == 0 && queued > 0 ? " — nothing acknowledged: is anything else awake on the bus?" : "");
  printHeard("");
  if (refused > 0 && lastProblem && refusedInARow < kMaxRefusedInARow) {
    std::printf("last refusal: %s\n", lastProblem->c_str());
  }
  logInfo(kTag, "repeating can-send of %s every %u ms ended: %u queued, %u refused", shown.c_str(),
          static_cast<unsigned>(command.repeatMs), static_cast<unsigned>(queued), static_cast<unsigned>(refused));
  return refusedInARow >= kMaxRefusedInARow ? 1 : 0;
}

// can-repeat: one frame resent in the background, so the console stays free (to dump, or to send
// something else while a node is held awake). One at a time; the same limits as can-send -r.
struct BackgroundRepeat {
  std::mutex mutex;
  TaskHandle_t task = nullptr;
  std::atomic<bool> stop{false};
  can::CanSendCommand command;
  std::atomic<std::uint32_t> queued{0};
  std::atomic<std::uint32_t> refused{0};
  std::int64_t startedUs = 0;
  std::string shown;
  std::string ended;  // why the last one stopped, for `can-repeat` to report
};
BackgroundRepeat background;

void backgroundRepeatTask(void*) {
  CanBus* bus = consoleBus();
  const can::ConsoleFrame frame = background.command.frame;
  const std::span<const std::uint8_t> payload(frame.data.data(), frame.length);
  const TickType_t period = std::max<TickType_t>(1, pdMS_TO_TICKS(background.command.repeatMs));
  std::uint32_t refusedInARow = 0;
  std::string why = "stopped from the console";
  TickType_t wake = xTaskGetTickCount();
  while (!background.stop.load(std::memory_order_acquire)) {
    if (const std::optional<std::string> problem = bus->sendRaw(frame.id, payload, frame.extended)) {
      background.refused.fetch_add(1, std::memory_order_relaxed);
      if (++refusedInARow >= kMaxRefusedInARow) {
        why = "gave up after " + std::to_string(kMaxRefusedInARow) + " refusals in a row (" + *problem + ")";
        break;
      }
    } else {
      background.queued.fetch_add(1, std::memory_order_relaxed);
      refusedInARow = 0;
    }
    if (esp_timer_get_time() - background.startedUs >= kMaxRepeatUs) {
      why = "stopped after 60 minutes";
      break;
    }
    vTaskDelayUntil(&wake, period);
  }
  logInfo(kTag, "background repeat of %s ended (%s): %u queued, %u refused", background.shown.c_str(), why.c_str(),
          static_cast<unsigned>(background.queued.load()), static_cast<unsigned>(background.refused.load()));
  {
    const std::lock_guard<std::mutex> lock(background.mutex);
    background.ended = why;
    background.task = nullptr;
  }
  // Only once the lock is released: a task deleted inside the guard never runs its destructor,
  // and the mutex stays locked for good — `can-repeat stop` then hangs the console.
  vTaskDelete(nullptr);
}

bool startBackground(const can::CanSendCommand& command, const char* busyHint);
bool stopBackground();

void reportBackground() {
  const std::lock_guard<std::mutex> lock(background.mutex);
  if (background.task == nullptr) {
    if (background.shown.empty()) {
      std::fputs("nothing is repeating\n", stdout);
    } else {
      std::printf("nothing is repeating; the last, %s every %u ms, %s after %u queued, %u refused\n",
                  background.shown.c_str(), static_cast<unsigned>(background.command.repeatMs),
                  background.ended.c_str(), static_cast<unsigned>(background.queued.load()),
                  static_cast<unsigned>(background.refused.load()));
    }
    return;
  }
  std::printf("repeating %s every %u ms for %llu s: %u queued, %u refused\n", background.shown.c_str(),
              static_cast<unsigned>(background.command.repeatMs),
              static_cast<unsigned long long>((esp_timer_get_time() - background.startedUs) / 1000000),
              static_cast<unsigned>(background.queued.load()), static_cast<unsigned>(background.refused.load()));
}

int canRepeat(int argumentCount, char** arguments) {
  if (argumentCount == 1) {
    reportBackground();
    return 0;
  }
  if (argumentCount == 2 && std::string_view(arguments[1]) == "stop") {
    if (!stopBackground()) {
      std::fputs("nothing is repeating\n", stdout);
      return 0;
    }
    reportBackground();
    return 0;
  }
  // The rest reads as can-send -r would: can-repeat <ms> <id> [byte ...].
  std::vector<const char*> asSend = {"can-send", "-r"};
  for (int at = 1; at < argumentCount; at++) asSend.push_back(arguments[at]);
  can::CanSendCommand command;
  if (const std::optional<std::string> error =
          can::parseCanSend(static_cast<int>(asSend.size()), asSend.data(), command)) {
    std::fputs("usage: can-repeat <ms> <id> [byte ...] | can-repeat stop | can-repeat\n", stdout);
    std::printf("%s\n", error->c_str());
    return 1;
  }
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  if (!bus->transmitting()) {
    std::fputs("not started: the bus is listen-only\n", stdout);
    return 1;
  }
  if (!startBackground(command, "`can-repeat stop` (or `psu off`)")) return 1;
  const std::string shown = can::formatCanFrame(
      command.frame.id, std::span<const std::uint8_t>(command.frame.data.data(), command.frame.length),
      command.frame.extended);
  logInfo(kTag, "background repeat of %s every %u ms started from the USB console", shown.c_str(),
          static_cast<unsigned>(command.repeatMs));
  std::printf("repeating %s every %u ms in the background; `can-repeat stop` ends it\n", shown.c_str(),
              static_cast<unsigned>(command.repeatMs));
  return 0;
}

// psu on | off | status: hold the PSU's main rail on by sending its enable request in the
// background, as the VCU does while the bike is on. Shares can-repeat's one background slot, so a
// hold and a can-repeat can't both run. The PSU drops the rail about a second after the request
// stops; the hold stops itself after 60 minutes, like any repeat.
const can::CanSendCommand& psuHoldCommand() {
  static const can::CanSendCommand command = [] {
    can::CanSendCommand built;
    built.frame.id = can::kPsuControlId;
    built.frame.length = static_cast<std::uint8_t>(can::kPsuEnableRequest.size());
    std::copy(can::kPsuEnableRequest.begin(), can::kPsuEnableRequest.end(), built.frame.data.begin());
    built.repeatMs = can::kPsuHoldIntervalMs;
    return built;
  }();
  return command;
}

bool isPsuHold(const can::CanSendCommand& command) {
  const can::CanSendCommand& hold = psuHoldCommand();
  return !command.frame.extended && command.frame.id == hold.frame.id && command.frame.length == hold.frame.length &&
         std::equal(hold.frame.data.begin(), hold.frame.data.begin() + hold.frame.length, command.frame.data.begin());
}

// Starts `command` in the background slot; the caller has checked the bus. False (and says why) if
// the slot is taken or the task can't start.
bool startBackground(const can::CanSendCommand& command, const char* busyHint) {
  const std::lock_guard<std::mutex> lock(background.mutex);
  if (background.task != nullptr) {
    std::printf("already repeating %s; %s first\n", background.shown.c_str(), busyHint);
    return false;
  }
  background.command = command;
  background.shown = can::formatCanFrame(command.frame.id,
                                         std::span<const std::uint8_t>(command.frame.data.data(), command.frame.length),
                                         command.frame.extended);
  background.queued.store(0);
  background.refused.store(0);
  background.stop.store(false);
  background.startedUs = esp_timer_get_time();
  background.ended.clear();
  if (xTaskCreate(backgroundRepeatTask, "can-repeat", 3072, nullptr, 5, &background.task) != pdPASS) {
    background.task = nullptr;
    std::fputs("no memory for the repeat task\n", stdout);
    return false;
  }
  return true;
}

// Stops whatever the background slot is sending and waits for it; false if nothing was.
bool stopBackground() {
  {
    const std::lock_guard<std::mutex> lock(background.mutex);
    if (background.task == nullptr) return false;
    background.stop.store(true, std::memory_order_release);
  }
  // The task notices within one period; wait for it so what follows is final.
  for (int waited = 0; waited < 700; waited++) {
    {
      const std::lock_guard<std::mutex> lock(background.mutex);
      if (background.task == nullptr) break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

// The hold's state: running (with how long), or not.
bool psuHoldRunning(std::uint64_t& forSeconds) {
  const std::lock_guard<std::mutex> lock(background.mutex);
  if (background.task == nullptr || !isPsuHold(background.command)) return false;
  forSeconds = static_cast<std::uint64_t>((esp_timer_get_time() - background.startedUs) / 1000000);
  return true;
}

void printPsuStatus() {
  std::uint64_t heldFor = 0;
  if (psuHoldRunning(heldFor)) {
    std::printf("hold: on for %llu s, sending 500 01 00 00 00 00 00 00 00 every %u ms\n",
                static_cast<unsigned long long>(heldFor), static_cast<unsigned>(can::kPsuHoldIntervalMs));
  } else {
    std::fputs("hold: off\n", stdout);
  }
  const PsuSnapshot snapshot = psuSnapshot();
  const std::int64_t nowUs = esp_timer_get_time();
  // The PSU sends both at ~10 Hz while it is awake; older than this, it has gone quiet.
  constexpr std::int64_t kFreshUs = 2 * 1000 * 1000;
  const bool monitorFresh = snapshot.monitorAtUs != 0 && nowUs - snapshot.monitorAtUs < kFreshUs;
  const bool statusFresh = snapshot.statusAtUs != 0 && nowUs - snapshot.statusAtUs < kFreshUs;
  if (!monitorFresh && !statusFresh) {
    if (snapshot.monitorAtUs == 0 && snapshot.statusAtUs == 0) {
      std::fputs("psu: not heard since boot (asleep, or not on this bus)\n", stdout);
    } else {
      const std::int64_t lastUs = std::max(snapshot.monitorAtUs, snapshot.statusAtUs);
      std::printf("psu: quiet for %.1f s (asleep: the rail is off)\n", static_cast<double>(nowUs - lastUs) / 1e6);
    }
    return;
  }
  if (statusFresh && snapshot.statusLength >= 1) {
    const std::string raw =
        can::formatCanFrame(can::kPsuStatusId, std::span<const std::uint8_t>(snapshot.status.data(), snapshot.statusLength));
    std::printf("psu: state %02X (%s)   [%s]\n", snapshot.status[0], can::describePsuState(snapshot.status[0]),
                raw.c_str());
  }
  if (monitorFresh) {
    if (const std::optional<can::PsuMonitor> monitor = can::decodePsuMonitor(
            std::span<const std::uint8_t>(snapshot.monitor.data(), snapshot.monitorLength))) {
      std::printf("rail: main %.2f V (%s), standby %.2f V, load %.2f A\n", monitor->mainMv / 1000.0,
                  monitor->mainMv >= can::kPsuRailOnMv ? "on" : "off", monitor->standbyMv / 1000.0,
                  monitor->loadMa / 1000.0);
    }
  }
}

int psu(int argumentCount, char** arguments) {
  const std::string_view choice = argumentCount >= 2 ? std::string_view(arguments[1]) : std::string_view("status");
  if (argumentCount > 2 || (choice != "on" && choice != "off" && choice != "status")) {
    std::fputs("usage: psu [on | off | status]\n", stdout);
    return 1;
  }
  if (choice == "status") {
    printPsuStatus();
    return 0;
  }
  if (choice == "off") {
    {
      const std::lock_guard<std::mutex> lock(background.mutex);
      if (background.task != nullptr && !isPsuHold(background.command)) {
        std::printf("not holding the PSU: the background slot is repeating %s (`can-repeat stop` ends that)\n",
                    background.shown.c_str());
        return 1;
      }
    }
    if (!stopBackground()) {
      std::fputs("the hold was already off\n", stdout);
      return 0;
    }
    logInfo(kTag, "PSU hold stopped from the USB console");
    std::fputs("hold stopped; the PSU drops its main rail in about a second\n", stdout);
    return 0;
  }
  // on
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  if (!bus->transmitting()) {
    std::fputs("not started: the bus is listen-only (set OBD_ENABLED 1 and restart)\n", stdout);
    return 1;
  }
  std::uint64_t heldFor = 0;
  if (psuHoldRunning(heldFor)) {
    std::printf("already holding the PSU on (%llu s)\n", static_cast<unsigned long long>(heldFor));
    return 0;
  }
  if (!startBackground(psuHoldCommand(), "`can-repeat stop`")) return 1;
  logInfo(kTag, "PSU hold started from the USB console");
  std::printf("holding the PSU on: 500 01 00 00 00 00 00 00 00 every %u ms (stops itself after 60 min)\n",
              static_cast<unsigned>(can::kPsuHoldIntervalMs));
  // Wait for the PSU to answer, up to 2 s, so the reply says whether the rail came up.
  const std::int64_t startedUs = esp_timer_get_time();
  while (esp_timer_get_time() - startedUs < 2 * 1000 * 1000) {
    vTaskDelay(pdMS_TO_TICKS(100));
    const PsuSnapshot snapshot = psuSnapshot();
    if (snapshot.statusAtUs > startedUs && snapshot.statusLength >= 1 && snapshot.status[0] == 0x03) break;
  }
  printPsuStatus();
  return 0;
}

int canSend(int argumentCount, char** arguments) {
  can::CanSendCommand command;
  if (const std::optional<std::string> error = can::parseCanSend(argumentCount, arguments, command)) {
    std::printf("%s\n", error->c_str());
    return 1;
  }
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  const can::ConsoleFrame& frame = command.frame;
  const std::span<const std::uint8_t> payload(frame.data.data(), frame.length);
  const std::string shown = can::formatCanFrame(frame.id, payload, frame.extended);
  if (!bus->transmitting()) {
    std::printf("%s not sent: the bus is listen-only\n", shown.c_str());
    return 1;
  }
  if (command.repeatMs > 0) return sendRepeatedly(*bus, command, payload, shown);
  if (const std::optional<std::string> problem = bus->sendRaw(frame.id, payload, frame.extended)) {
    std::printf("%s not sent: %s\n", shown.c_str(), problem->c_str());
    return 1;
  }
  // Queued, not yet acknowledged: the TWAI driver retries until something ACKs it. The status
  // report's sent / never-acknowledged counts say how it went.
  std::printf("%s queued\n", shown.c_str());
  return 0;
}

// can-dump -c: per id, what has been seen, for printing changes and the closing summary.
struct SeenId {
  std::uint32_t id;
  bool extended;
  std::uint32_t count;
  std::int64_t firstUs;
  std::int64_t lastPrintedUs;
  std::uint8_t length;
  std::uint8_t data[8];
  bool unprinted;  // changed since it was last printed (held back by the rate cap)
};

// A changing id is printed at most this often, so a bus of fast-moving signals still fits the
// serial link; its first appearance is always printed.
constexpr std::int64_t kChangePrintIntervalUs = 500 * 1000;
constexpr std::size_t kMaxSeenIds = 128;

int canDump(int argumentCount, char** arguments) {
  bool changesOnly = false;
  std::vector<const char*> rest = {arguments[0]};
  for (int at = 1; at < argumentCount; at++) {
    if (std::string_view(arguments[at]) == "-c") {
      changesOnly = true;
    } else {
      rest.push_back(arguments[at]);
    }
  }
  std::optional<can::CanId> filter;
  if (const std::optional<std::string> error =
          can::parseCanDumpFilter(static_cast<int>(rest.size()), rest.data(), filter)) {
    std::printf("%s\n", error->c_str());
    return 1;
  }
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  std::vector<SeenId> seen;
  if (changesOnly) seen.reserve(kMaxSeenIds);
  if (!bus->startDump()) {
    std::fputs("no memory for the dump\n", stdout);
    return 1;
  }
  const char* mode = changesOnly ? " (new ids and changed payloads only, each id at most every 500 ms)" : "";
  if (filter) {
    std::printf(filter->extended ? "dumping id %08X%s — press Enter to stop\n" : "dumping id %03X%s — press Enter to stop\n",
                static_cast<unsigned>(filter->id), mode);
  } else {
    std::printf("dumping every frame%s — press Enter to stop\n", mode);
  }
  std::fputs("time is seconds since the dump started\n", stdout);
  std::fflush(stdout);

  // Remote frames never reach the dump (the receive path drops them), so count them instead, or a
  // bus carrying only those would look silent.
  const CanBusCounters& counters = bus->counters();
  const std::uint32_t remoteBefore = counters.ignoredRemote.load(std::memory_order_relaxed);
  const std::int64_t startedUs = esp_timer_get_time();
  std::uint32_t untracked = 0;

  const auto print = [&](const ReceivedFrame& frame, const char* tag) {
    const std::string line =
        can::formatCanFrame(frame.id, std::span<const std::uint8_t>(frame.data, frame.length), frame.extended);
    const std::int64_t at = frame.arrivedUs - startedUs;
    std::printf("%4lld.%03lld %s%s\n", static_cast<long long>(at / 1000000), static_cast<long long>((at / 1000) % 1000),
                line.c_str(), tag);
  };

  // Watch stdin without blocking, so the dump can be stopped by a keypress while it waits on frames.
  KeyWatch keys;
  std::int64_t nextKeyCheckUs = 0;
  while (true) {
    ReceivedFrame frame;
    if (bus->nextDumpFrame(frame, pdMS_TO_TICKS(50))) {
      if (!filter || (frame.id == filter->id && frame.extended == filter->extended)) {
        if (!changesOnly) {
          print(frame, "");
        } else {
          auto found = std::find_if(seen.begin(), seen.end(), [&](const SeenId& entry) {
            return entry.id == frame.id && entry.extended == frame.extended;
          });
          if (found == seen.end()) {
            if (seen.size() < kMaxSeenIds) {
              SeenId entry = {frame.id, frame.extended, 1, frame.arrivedUs, frame.arrivedUs, frame.length, {}, false};
              std::memcpy(entry.data, frame.data, frame.length);
              seen.push_back(entry);
              print(frame, "  (new)");
            } else {
              untracked++;
            }
          } else {
            found->count++;
            const bool changed =
                found->length != frame.length || std::memcmp(found->data, frame.data, frame.length) != 0;
            if (changed || found->unprinted) {
              found->length = frame.length;
              std::memcpy(found->data, frame.data, frame.length);
              if (frame.arrivedUs - found->lastPrintedUs >= kChangePrintIntervalUs) {
                found->lastPrintedUs = frame.arrivedUs;
                found->unprinted = false;
                print(frame, "");
              } else {
                found->unprinted = true;
              }
            }
          }
        }
      }
    }
    // Checking stdin is a syscall; at a few thousand frames a second, don't do it per frame.
    const std::int64_t nowUs = esp_timer_get_time();
    if (nowUs >= nextKeyCheckUs) {
      nextKeyCheckUs = nowUs + 50 * 1000;
      std::fflush(stdout);
      if (keys.pressed()) break;
    }
  }

  bus->stopDump();
  const std::int64_t endedUs = esp_timer_get_time();
  if (changesOnly) {
    std::sort(seen.begin(), seen.end(), [](const SeenId& a, const SeenId& b) { return a.firstUs < b.firstUs; });
    std::printf("summary: %u ids over %.1f s, by first appearance (first at, id, frames, rate, last payload)\n",
                static_cast<unsigned>(seen.size()), static_cast<double>(endedUs - startedUs) / 1e6);
    for (const SeenId& entry : seen) {
      const double span = static_cast<double>(endedUs - entry.firstUs) / 1e6;
      const std::string line =
          can::formatCanFrame(entry.id, std::span<const std::uint8_t>(entry.data, entry.length), entry.extended);
      std::printf("  %8.3f  %s  x%u  %.0f/s\n", static_cast<double>(entry.firstUs - startedUs) / 1e6, line.c_str(),
                  static_cast<unsigned>(entry.count), span > 0 ? entry.count / span : 0.0);
    }
    if (untracked > 0) std::printf("  and %u frames of ids past the first %u\n", static_cast<unsigned>(untracked),
                                   static_cast<unsigned>(kMaxSeenIds));
  }
  const unsigned remote = counters.ignoredRemote.load(std::memory_order_relaxed) - remoteBefore;
  if (remote > 0) std::printf("not shown: %u remote frames (only data frames are printed)\n", remote);
  const std::uint32_t dropped = bus->dumpDropped();
  if (dropped > 0) {
    std::printf("stopped; %u frames were dropped (printing couldn't keep up with the bus)\n",
                static_cast<unsigned>(dropped));
  } else {
    std::fputs("stopped\n", stdout);
  }
  return 0;
}

int poller(int argumentCount, char** arguments) {
  if (consoleDiagnosticsTask == nullptr) {
    std::fputs("diagnostics aren't running (the boot log says why)\n", stdout);
    return 1;
  }
  if (argumentCount == 1) {
    const std::optional<bool> running = consoleDiagnosticsTask->pollerRunning();
    if (!running) {
      std::fputs(kNoPoller, stdout);
    } else {
      std::printf("the OBD poller is %s\n", *running ? "running" : "stopped");
    }
    return 0;
  }
  if (argumentCount != 2) {
    std::fputs("usage: poller [on | off]\n", stdout);
    return 1;
  }
  const std::string_view choice = arguments[1];
  bool run = false;
  if (choice == "on" || choice == "start" || choice == "1") {
    run = true;
  } else if (choice == "off" || choice == "stop" || choice == "0") {
    run = false;
  } else {
    std::fputs("usage: poller [on | off]\n", stdout);
    return 1;
  }
  if (!consoleDiagnosticsTask->setPollerRunning(run)) {
    std::fputs(kNoPoller, stdout);
    return 1;
  }
  logInfo(kTag, "the OBD poller was %s from the USB console", run ? "started" : "stopped");
  std::printf("the OBD poller is now %s\n", run ? "running" : "stopped");
  return 0;
}

}  // namespace

void setConsoleDiagnostics(DiagnosticsTask* diagnostics) { consoleDiagnosticsTask = diagnostics; }

bool registerCanConsoleCommands() {
  const esp_console_cmd_t commands[] = {
      {.command = "can-send",
       .help = "Put one CAN frame on the bus: can-send <id> [byte ...], hex, up to 8 data bytes "
               "(e.g. can-send 7DF 02 01 0C). An id of 4-8 digits is extended (29-bit), e.g. 18DAA0F1. -r <ms> resends it every <ms> milliseconds until a key is "
               "pressed (e.g. can-send -r 150 500 01 00 00 00 00 00 00 00). Needs OBD_ENABLED; the USB "
               "console only.",
       .hint = "[-r <ms>] <id> [byte ...]", .func = canSend, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "can-repeat",
       .help = "Resend one CAN frame in the background, leaving the console free: can-repeat <ms> <id> [byte ...] "
               "(e.g. can-repeat 150 500 01 00 00 00 00 00 00 00). `can-repeat stop` ends it, `can-repeat` alone "
               "reports it. One at a time; stops itself after 60 minutes or 3 refusals in a row. Needs OBD_ENABLED; "
               "the USB console only.",
       .hint = "<ms> <id> [byte ...] | stop", .func = canRepeat, .argtable = nullptr, .func_w_context = nullptr,
       .context = nullptr},
      {.command = "psu",
       .help = "Hold the PSU's 12 V main rail on from here, as the VCU does: `psu on` sends its enable request "
               "(500 01 00 00 00 00 00 00 00) every 150 ms in the background, `psu off` stops it (the rail drops "
               "about a second later), `psu status` (or `psu`) shows the hold and the PSU's own state and rail "
               "voltages. Stops itself after 60 minutes. Needs OBD_ENABLED; the USB console only.",
       .hint = "[on | off | status]", .func = psu, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "can-dump",
       .help = "Print CAN frames, 29-bit ones too, as they arrive until a key is pressed, each with its time; "
               "can-dump <id> shows only that id. -c prints only new ids and changed payloads, and a summary "
               "of every id at the end. "
               "The USB console only.",
       .hint = "[-c] [id]", .func = canDump, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "poller",
       .help = "Stop or restart the OBD poller: `poller off` frees the bus for can-send, `poller on` resumes it; "
               "`poller` alone says which it is. Needs OBD_ENABLED.",
       .hint = "[on | off]", .func = poller, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
  };
  for (const esp_console_cmd_t& command : commands) {
    const esp_err_t result = esp_console_cmd_register(&command);
    if (result != ESP_OK) {
      logError(kTag, "registering `%s` failed: %s", command.command, esp_err_to_name(result));
      return false;
    }
  }
  return true;
}

}  // namespace emcool
