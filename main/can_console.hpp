#pragma once

// The USB serial console's manual CAN commands, registered only on the UART console (never on the
// Wi-Fi or Bluetooth consoles, which reach the bike's bus and could be misused over the network):
//
//   can-send [-r <ms>] <id> [byte ...]
//                              put one standard-id frame on the bus, or with -r resend it every
//                              <ms> ms until a key is pressed (needs OBD_ENABLED)
//   can-repeat <ms> <id> [byte ...] | stop
//                              resend one frame in the background, leaving the console free
//   can-dump [id]              print frames as they arrive, until a key is pressed
//   poller [on | off]          stop or restart the OBD poller, so manual sends don't clash with it
//   psu [on | off | status]    hold the PSU's 12 V rail on, as the VCU does; shares can-repeat's slot
//
// app_main wires the bus and the diagnostics task in with setConsoleBus() (can_bus.hpp) and
// setConsoleDiagnostics() below, before the console starts.

#include "diagnostics_task.hpp"

namespace emcool {

// The diagnostics task the `poller` command drives; null until set, which keeps `poller` inert
// until OBD exists.
void setConsoleDiagnostics(DiagnosticsTask* diagnostics);

// Registers can-send, can-repeat, can-dump, poller and psu on the UART console. Call from startSettingsConsole.
bool registerCanConsoleCommands();

}  // namespace emcool
