#include "decoders.hpp"

// The charge manager's five ids, one ECU's state read together: 0x605 which path the
// session is on, 0x610 the handshake, 0x615 measures it, 0x620 bounds it, 0x625 whether
// anything flows. Ported from cool-eva src/can/charge-manager.ts, which has the evidence.

namespace emcool::can {

namespace {

constexpr std::uint32_t kChargeBmsCommandCanId = 0x605;
constexpr std::uint32_t kChargeStateCanId = 0x610;
constexpr std::uint32_t kChargeTelemetryCanId = 0x615;
constexpr std::uint32_t kChargeLimitsCanId = 0x620;
constexpr std::uint32_t kChargeConfigCanId = 0x625;
constexpr std::uint8_t kChargeManagerFamilyByte = 0xf1;

}  // namespace

bool isChargeManagerCanId(std::uint32_t id) {
  return id == kChargeBmsCommandCanId || id == kChargeStateCanId || id == kChargeTelemetryCanId ||
         id == kChargeLimitsCanId || id == kChargeConfigCanId;
}

void decodeChargeManagerFrame(std::uint32_t id, FrameBytes data, DecodedValues& out) {
  switch (id) {
    case kChargeBmsCommandCanId:
      if (data.size() < 8) return;
      out.push("charge_type", data[2]);
      out.push("bms_leak_detect_inhibit", data[7]);
      return;

    case kChargeStateCanId:
      if (data.size() < 8) return;
      if (data[4] != kChargeManagerFamilyByte) return;
      out.push("charge_manager_status", data[0]);
      out.push("charge_manager_state", data[7]);
      out.push("charge_manager_error_src", data[1]);
      out.push("charge_manager_error_code", i16le(data[2], data[3]));
      return;

    case kChargeTelemetryCanId:
      if (data.size() < 8) return;
      if (data[1] != 0x01 || data[4] != 0 || data[5] != 0 || data[6] != 0 || data[7] != 0) return;
      // Kept although its original reason is gone: b0 = 0 is just a 256 V target under the
      // 16-bit reading, and has never occurred. cool-eva src/can/charge-manager.ts.
      if (data[0] != 0) {
        out.push("fast_dc_target_v", u16le(data[0], data[1]));
      }
      out.push("fast_dc_target_a", data[2]);
      out.push("charge_manager_soc", data[3]);
      return;

    case kChargeLimitsCanId:
      if (data.size() < 8) return;
      if (data[3] == 0 || data[4] != 0 || data[5] != 0 || data[6] != 0 || data[7] != 0) return;
      out.push("fast_dc_limit_a", data[0]);
      out.push("ac_supply_limit_a", data[1]);
      return;

    case kChargeConfigCanId: {
      if (data.size() < 8) return;
      if (data[1] != 0x01 || data[3] != 0xff) return;
      const std::uint8_t flags = data[4];
      out.push("fast_dc_limit_max_v", u16le(data[0], data[1]));
      out.push("fast_dc_limit_max_a", data[2]);
      // Inverted: bit 5 is set when DC current is NOT flowing. Both bits mean current is
      // flowing, not that a session exists — that is `charge_type`.
      out.push("dc_charging", bit(flags, 5) ? 0 : 1);
      out.push("ac_charging", bit(flags, 2));
      return;
    }

    default:
      return;
  }
}

}  // namespace emcool::can
