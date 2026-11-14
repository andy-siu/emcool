#include "can_console.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "can_bus.hpp"
#include "emcool/can/console.hpp"
#include "emcool/log.hpp"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace emcool {

namespace {

constexpr const char* kTag = "can-console";

DiagnosticsTask* consoleDiagnosticsTask = nullptr;

const char* kNoBus = "the CAN bus isn't running (the boot log says why)\n";
const char* kNoPoller = "OBD is off (set OBD_ENABLED 1 and restart); there is no poller\n";

int canSend(int argumentCount, char** arguments) {
  can::ConsoleFrame frame;
  if (const std::optional<std::string> error = can::parseCanSend(argumentCount, arguments, frame)) {
    std::printf("%s\n", error->c_str());
    return 1;
  }
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  const std::span<const std::uint8_t> payload(frame.data.data(), frame.length);
  const std::string shown = can::formatCanFrame(frame.id, payload);
  if (const std::optional<std::string> problem = bus->sendRaw(frame.id, payload)) {
    std::printf("%s not sent: %s\n", shown.c_str(), problem->c_str());
    return 1;
  }
  // Queued, not yet acknowledged: the TWAI driver retries until something ACKs it. The status
  // report's sent / never-acknowledged counts say how it went.
  std::printf("%s queued\n", shown.c_str());
  return 0;
}

int canDump(int argumentCount, char** arguments) {
  std::optional<std::uint32_t> filter;
  if (const std::optional<std::string> error = can::parseCanDumpFilter(argumentCount, arguments, filter)) {
    std::printf("%s\n", error->c_str());
    return 1;
  }
  CanBus* bus = consoleBus();
  if (bus == nullptr) {
    std::fputs(kNoBus, stdout);
    return 1;
  }
  if (!bus->startDump()) {
    std::fputs("no memory for the dump\n", stdout);
    return 1;
  }
  if (filter) {
    std::printf("dumping id %03X — press Enter to stop\n", static_cast<unsigned>(*filter));
  } else {
    std::fputs("dumping every frame — press Enter to stop\n", stdout);
  }
  std::fflush(stdout);

  // Watch stdin without blocking, so the dump can be stopped by a keypress while it waits on frames.
  const int stdinFd = fileno(stdin);
  const int savedFlags = fcntl(stdinFd, F_GETFL, 0);
  if (savedFlags != -1) fcntl(stdinFd, F_SETFL, savedFlags | O_NONBLOCK);

  while (true) {
    ReceivedFrame frame;
    if (bus->nextDumpFrame(frame, pdMS_TO_TICKS(200))) {
      if (!filter || frame.id == *filter) {
        const std::string line = can::formatCanFrame(frame.id, std::span<const std::uint8_t>(frame.data, frame.length));
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
      }
    }
    char typed = 0;
    bool stop = false;
    while (read(stdinFd, &typed, 1) > 0) stop = true;
    if (stop) break;
  }

  if (savedFlags != -1) fcntl(stdinFd, F_SETFL, savedFlags);
  bus->stopDump();
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
       .help = "Put one standard-id CAN frame on the bus: can-send <id> [byte ...], hex, up to 8 data bytes "
               "(e.g. can-send 7DF 02 01 0C). Needs OBD_ENABLED; the USB console only.",
       .hint = "<id> [byte ...]", .func = canSend, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
      {.command = "can-dump",
       .help = "Print CAN frames as they arrive until a key is pressed; can-dump <id> shows only that id. "
               "The USB console only.",
       .hint = "[id]", .func = canDump, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
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
