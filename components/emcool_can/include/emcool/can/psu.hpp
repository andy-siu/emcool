#pragma once

// The PSU (the DC-DC converter that makes the bike's 12 V from the traction pack), as the
// serial console's `psu` command sees it: its request frame, and what its two status frames say.
// Pure, so the host checks run it.
//
//   0x500 PSU_Ctrl     b0 bit 0 P_ENABLE_REQ. The VCU sends 01 00 00 00 00 00 00 00 at ~10 Hz
//                      while the bike is on; the PSU holds its main rail only while it keeps
//                      arriving, and drops it about a second after it stops (measured on the bike).
//   0x501 PSU_Monitor  b0-1 main rail mV, b2-3 standby rail mV, b4-5 12 V load mA, all LE.
//   0x502 PSU_Sts      b0 the PSU's state. Seen: 00 awake with the rail off, 03 rail on, 02 on a
//                      bench unit that never switched its rail on. Nothing documents the rest.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace emcool::can {

inline constexpr std::uint32_t kPsuControlId = 0x500;
inline constexpr std::uint32_t kPsuMonitorId = 0x501;
inline constexpr std::uint32_t kPsuStatusId = 0x502;

// What the VCU sends to keep the rail on, byte for byte.
inline constexpr std::array<std::uint8_t, 8> kPsuEnableRequest = {0x01, 0, 0, 0, 0, 0, 0, 0};

// The VCU's own rate is ~100 ms; the reference's hold uses 150 ms, still well inside the ~1 s
// the PSU waits before dropping the rail.
inline constexpr std::uint32_t kPsuHoldIntervalMs = 150;

// A rail at or above this is "on": the bike's reads 12.6-12.8 V, and an off one a few hundred mV.
inline constexpr std::uint32_t kPsuRailOnMv = 11000;

struct PsuMonitor {
  std::uint32_t mainMv = 0;
  std::uint32_t standbyMv = 0;
  std::uint32_t loadMa = 0;
};

// nullopt for a frame too short to carry the three channels.
std::optional<PsuMonitor> decodePsuMonitor(std::span<const std::uint8_t> data);

// The 0x502 state byte as words: "rail off", "rail on", "not starting", or "unknown".
const char* describePsuState(std::uint8_t state);

}  // namespace emcool::can
