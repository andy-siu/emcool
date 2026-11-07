#pragma once

// The console over Wi-Fi (port 23): one client at a time, behind ADMIN_PASSWORD, then the same
// commands as the USB console. This is the part with no sockets in it — the bytes in, the bytes out,
// the login and its lockout — so the host checks run it; main/wifi_console.cpp feeds it a TCP socket.
//
// Plain TCP, so `nc <bike> 23` works (macOS has no telnet). Telnet clients' option negotiation
// (IAC …) is stepped over. Lines end at CR or LF; a CR LF pair is one line. Backspace edits the line.
// The password is the only thing it protects with: it crosses the network in plain text, as does
// everything after it, and `set ADMIN_PASSWORD` over this console sends the new one in plain text too.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace emcool::settings {

class RemoteConsole {
 public:
  // After this many wrong passwords in a row, every attempt waits kLockoutMs first (each client's too).
  static constexpr int kFreeAttempts = 3;
  static constexpr std::int64_t kLockoutMs = 5000;
  static constexpr int kAttemptsPerConnection = 3;
  // A line longer than this is refused whole; a command needs far less, a secret 128 characters.
  static constexpr std::size_t kMaxLineBytes = 512;

  // Runs one command line (already split into words), writing what it prints into `output`.
  using RunCommand = std::function<void(int argumentCount, char** arguments, std::string& output)>;

  RemoteConsole(std::string_view password, RunCommand run) : password_(password), run_(std::move(run)) {}

  // A client connected: the greeting and the password prompt.
  std::string connected(std::int64_t nowMs);
  // Bytes from the client; what to send back. `close` is set once the connection should end.
  std::string receive(std::span<const char> bytes, std::int64_t nowMs, bool& close);
  bool authenticated() const { return authenticated_; }

 private:
  std::string line(std::string_view text, std::int64_t nowMs, bool& close);
  std::string login(std::string_view attempt, std::int64_t nowMs, bool& close);
  std::string prompt() const;

  std::string password_;
  RunCommand run_;
  // Per connection.
  bool authenticated_ = false;
  int attemptsThisConnection_ = 0;
  std::string pending_;
  bool overlong_ = false;
  bool lastWasCr_ = false;
  enum class Telnet : std::uint8_t { kData, kCommand, kOption, kSubnegotiation, kSubnegotiationIac };
  Telnet telnet_ = Telnet::kData;
  // Across connections.
  int failuresInARow_ = 0;
  std::int64_t lockedUntilMs_ = 0;
};

// The words of a line, as the USB console splits them: spaces separate, double quotes group, and a
// backslash escapes the next character.
std::vector<std::string> splitWords(std::string_view line);

// Equal in time that doesn't depend on where they differ.
bool constantTimeEquals(std::string_view left, std::string_view right);

// The password from an HTTP Basic `Authorization` header ("Basic base64(user:password)"); the user
// name is ignored. nullopt if the header isn't one.
std::optional<std::string> basicAuthPassword(std::string_view header);

// Text for a network terminal: every LF as CR LF.
std::string withCrLf(std::string_view text);

}  // namespace emcool::settings
