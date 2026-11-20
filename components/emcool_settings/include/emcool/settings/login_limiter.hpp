#pragma once

// The SSH console's password checks: three tries a connection, and after three wrong in a row (from
// any connection) every attempt is refused for kLockoutMs, the right password too. Platform-free, so
// the host checks run it.

#include <cstdint>
#include <string_view>

namespace emcool::settings {

class LoginLimiter {
 public:
  static constexpr int kFreeAttempts = 3;
  static constexpr std::int64_t kLockoutMs = 5000;
  static constexpr int kAttemptsPerConnection = 3;

  enum class Verdict : std::uint8_t {
    kAccepted,
    kWrong,      // wrong; the client may try again
    kWrongLast,  // wrong, and that was this connection's last try
    kLockedOut,  // not even checked: too many wrong in a row, too recently
  };

  // A new connection: its tries start again (the lockout doesn't).
  void connected() { attemptsThisConnection_ = 0; }
  Verdict check(std::string_view attempt, std::string_view password, std::int64_t nowMs);

 private:
  int attemptsThisConnection_ = 0;
  int failuresInARow_ = 0;
  std::int64_t lockedUntilMs_ = 0;
};

}  // namespace emcool::settings
