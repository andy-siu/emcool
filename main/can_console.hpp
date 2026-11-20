#pragma once

// The manual CAN commands, on the USB console and the SSH console only (never the read-only telnet
// console or the dashboard's Bike console, which anyone holding a plain-text password or a stolen
// phone could reach):
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

#include <span>
#include <string_view>

#include "console_io.hpp"
#include "diagnostics_task.hpp"
#include "emcool/text/help.hpp"

namespace emcool {

// The diagnostics task the `poller` command drives; null until set, which keeps `poller` inert
// until OBD exists.
void setConsoleDiagnostics(DiagnosticsTask* diagnostics);

// Registers can-send, can-repeat, can-dump, poller and psu on the USB console. Call from startSettingsConsole.
bool registerCanConsoleCommands();

// For the SSH console: whether `name` is one of them, and running one, printing into `io`.
bool isCanCommand(std::string_view name);
int runCanCommand(ConsoleIo& io, int argumentCount, char** arguments);
// Their lines in a console's `help`.
std::span<const text::HelpLine> canCommandHelp();

// The USB console's ConsoleIo: stdout, and keys read from stdin (settings_console.cpp).
ConsoleIo& usbConsoleIo();

}  // namespace emcool
