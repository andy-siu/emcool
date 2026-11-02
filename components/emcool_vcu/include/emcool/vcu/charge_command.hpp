#pragma once

// The 0x120/0x121 dash-command frames, ported from cool-eva src/can/charge-command.ts and
// src/can/charge-soc-command.ts: the SOC charge-limit write/read (stage 4) and the charge-current
// command and charge-stop (stage 5).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emcool::vcu {

inline constexpr std::uint32_t kChargeRequestCanId = 0x120;
inline constexpr std::uint32_t kChargeCommandCanId = 0x121;
inline constexpr std::uint8_t kSeparatorByte = 0xff;
inline constexpr std::uint8_t kRequestTwinBit = 0x80;
inline constexpr std::uint8_t kChargeSocLimitOpcode = 0x2c;
inline constexpr int kMaxSocLimitPct = 100;

// The one-frame SOC-limit write on 0x120; nullopt if percent is outside 0..100.
std::optional<std::array<std::uint8_t, 8>> buildChargeSocLimitWrite(int percent);
// The one-frame SOC-limit read on 0x120 (bit 7 clear).
std::array<std::uint8_t, 8> buildChargeSocLimitRead();

enum class ChargeMode : std::uint8_t { kAc, kDc };
const char* chargeModeName(ChargeMode mode);          // "ac" | "dc"
const char* chargeModeNameUpper(ChargeMode mode);     // "AC" | "DC"

struct ChargeFrame {
  std::uint32_t id;
  std::array<std::uint8_t, 8> data;
};

// buildChargeCurrentCommand: the 0x120 request-twin commit then the 0x121 command. `error` is set
// (and the result empty) when the amps/ceiling are out of range, as cool-eva throws.
std::vector<ChargeFrame> buildChargeCurrentCommand(ChargeMode mode, int selectedAmps, int ceilingAmps,
                                                   std::string& error);
// buildChargeStopCommand: the one 0x120 request-twin that commits a Mode-stop.
std::vector<ChargeFrame> buildChargeStopCommand();

}  // namespace emcool::vcu
