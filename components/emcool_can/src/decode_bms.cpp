#include <initializer_list>

#include "decoders.hpp"

// The BMS's own frames, decoded from its decrypted config file rather than reverse
// engineered.

namespace emcool::can {

namespace {

constexpr unsigned kLmuCount = 11;

struct SystemStateBit {
  std::uint8_t mask;
  const char* key;
};

constexpr SystemStateBit kSystemStateBits[] = {
    {0x01, "bms_state_discharge"}, {0x02, "bms_state_charge"},
    {0x04, "bms_state_balancing"}, {0x08, "bms_state_trickle"},
    {0x10, "bms_state_idle"},      {0x20, "bms_state_charge_complete"},
    {0x40, "bms_state_maintenance"},  // BMS firmware v6.16+ only
};

constexpr std::uint32_t kOverTemperatureErrorMask = (1u << 4) | (1u << 7);  // cell | LMU
// main +/−, precharge, midpack, precharge timeout
constexpr std::uint32_t kContactorErrorMask = (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25);

// 0x660 is DLC 3 before the BMS's VCU temperature offset is configured and DLC 8 after. The
// clamp bytes need the full frame, a stricter gate than the true temperatures: two decisions.
constexpr std::size_t kOffsetConfigMinDlc = 5;
constexpr std::size_t kClampInstrumentationMinDlc = 8;

constexpr unsigned kMinPlausibleCellMv = 1000;
constexpr unsigned kMaxPlausibleCellMv = 5000;
constexpr int kMinPlausibleLmuTempC = -40;
constexpr int kMaxPlausibleLmuTempC = 100;

#define EMCOOL_LMU_CELL_KEYS(lmu)                                                                    \
  {"lmu" #lmu "_cell1_mv", "lmu" #lmu "_cell2_mv", "lmu" #lmu "_cell3_mv", "lmu" #lmu "_cell4_mv", \
   "lmu" #lmu "_cell5_mv", "lmu" #lmu "_cell6_mv", "lmu" #lmu "_cell7_mv", "lmu" #lmu "_cell8_mv"}

// Indexed [lmu - 1][cell - 1]. LMUs 5-11 have 7 cells, so their cell-8 key is never used.
constexpr const char* kCellVoltageKeys[kLmuCount][8] = {
    EMCOOL_LMU_CELL_KEYS(1), EMCOOL_LMU_CELL_KEYS(2), EMCOOL_LMU_CELL_KEYS(3), EMCOOL_LMU_CELL_KEYS(4),
    EMCOOL_LMU_CELL_KEYS(5), EMCOOL_LMU_CELL_KEYS(6), EMCOOL_LMU_CELL_KEYS(7), EMCOOL_LMU_CELL_KEYS(8),
    EMCOOL_LMU_CELL_KEYS(9), EMCOOL_LMU_CELL_KEYS(10), EMCOOL_LMU_CELL_KEYS(11),
};
#undef EMCOOL_LMU_CELL_KEYS

enum LmuTemperatureSensor { kBattery1 = 0, kBoard1 = 1, kBoard2 = 2 };

#define EMCOOL_LMU_TEMPERATURE_KEYS(lmu) {"lmu" #lmu "_bat1_c", "lmu" #lmu "_pcb1_c", "lmu" #lmu "_pcb2_c"}

// Indexed [lmu - 1][LmuTemperatureSensor].
constexpr const char* kLmuTemperatureKeys[kLmuCount][3] = {
    EMCOOL_LMU_TEMPERATURE_KEYS(1), EMCOOL_LMU_TEMPERATURE_KEYS(2), EMCOOL_LMU_TEMPERATURE_KEYS(3),
    EMCOOL_LMU_TEMPERATURE_KEYS(4), EMCOOL_LMU_TEMPERATURE_KEYS(5), EMCOOL_LMU_TEMPERATURE_KEYS(6),
    EMCOOL_LMU_TEMPERATURE_KEYS(7), EMCOOL_LMU_TEMPERATURE_KEYS(8), EMCOOL_LMU_TEMPERATURE_KEYS(9),
    EMCOOL_LMU_TEMPERATURE_KEYS(10), EMCOOL_LMU_TEMPERATURE_KEYS(11),
};
#undef EMCOOL_LMU_TEMPERATURE_KEYS

unsigned cellsInLmu(unsigned lmuNumber) { return lmuNumber <= 4 ? 8 : 7; }

// BattTemp1Enabled is False on LMUs 6 and 8 in the BMS config: absence is configuration.
bool lmuHasBatteryTemperature(unsigned lmuNumber) { return lmuNumber != 6 && lmuNumber != 8; }

bool isPlausibleCellVoltage(unsigned millivolts) {
  return millivolts >= kMinPlausibleCellMv && millivolts <= kMaxPlausibleCellMv;
}

bool isPlausibleLmuTemperature(int celsius) {
  return celsius >= kMinPlausibleLmuTempC && celsius <= kMaxPlausibleLmuTempC;
}

// b0 is the LMU multiplexer; up to three u16 BE cell voltages follow it.
void decodeLmuCellVoltages(FrameBytes data, std::initializer_list<unsigned> cellNumbers, DecodedValues& out) {
  const unsigned lmuNumber = data[0];
  out.push("lmu_cell_mux", lmuNumber);
  if (lmuNumber < 1 || lmuNumber > kLmuCount) return;
  const unsigned cellsPresent = cellsInLmu(lmuNumber);
  unsigned slot = 0;
  for (const unsigned cellNumber : cellNumbers) {
    const unsigned slotIndex = slot;
    slot++;
    if (cellNumber > cellsPresent) continue;
    const unsigned millivolts = u16be(data[1 + slotIndex * 2], data[2 + slotIndex * 2]);
    if (!isPlausibleCellVoltage(millivolts)) continue;
    out.push(kCellVoltageKeys[lmuNumber - 1][cellNumber - 1], millivolts);
  }
}

void decodeLmuTemperatures(FrameBytes data, DecodedValues& out) {
  const unsigned lmuNumber = data[0];
  if (lmuNumber < 1 || lmuNumber > kLmuCount) return;
  const int board1 = signedByte(data[6]);
  if (isPlausibleLmuTemperature(board1)) {
    out.push(kLmuTemperatureKeys[lmuNumber - 1][kBoard1], board1);
  }
  const int board2 = signedByte(data[7]);
  if (isPlausibleLmuTemperature(board2)) {
    out.push(kLmuTemperatureKeys[lmuNumber - 1][kBoard2], board2);
  }
  if (!lmuHasBatteryTemperature(lmuNumber)) return;
  const int battery = signedByte(data[5]);
  if (isPlausibleLmuTemperature(battery)) {
    out.push(kLmuTemperatureKeys[lmuNumber - 1][kBattery1], battery);
  }
}

}  // namespace

void decodeBmsFrame(std::uint32_t id, FrameBytes data, DecodedValues& out) {
  switch (id) {
    case 0x200: {
      if (data.size() < 8) return;
      const double packVolts = u16be(data[4], data[5]) / 10.0;
      const double packAmps = i16be(data[6], data[7]) / 10.0;
      out.push("batt_temp_lo_vcu", signedByte(data[0]));
      out.push("soc", data[1]);
      out.push("soh", data[2]);
      out.push("batt_temp_hi_vcu", signedByte(data[3]));
      out.push("pack_v", packVolts);
      out.push("pack_a", packAmps);
      out.push("pack_kw", jsRound(((packVolts * packAmps) / 1000.0) * 1000.0) / 1000.0);
      return;
    }

    case 0x201: {
      if (data.size() < 1) return;
      const std::uint8_t systemState = data[0];
      out.push("charge_state", systemState);
      for (const SystemStateBit& stateBit : kSystemStateBits) {
        out.push(stateBit.key, (systemState & stateBit.mask) ? 1 : 0);
      }
      if (data.size() < 8) return;
      const std::uint32_t errorFlags = u32be(data, 1);
      const std::uint32_t warningFlags = u24be(data, 5);
      out.push("bms_error_flags", errorFlags);
      out.push("bms_warning_flags", warningFlags);
      out.push("bms_err_cell_overvoltage", bit(errorFlags, 0));
      out.push("bms_err_cell_undervoltage", bit(errorFlags, 1));
      out.push("bms_err_over_temp", (errorFlags & kOverTemperatureErrorMask) ? 1 : 0);
      out.push("bms_err_leak_detected", bit(errorFlags, 16));
      out.push("bms_err_leak_detect_failed", bit(errorFlags, 17));
      out.push("bms_err_contactor", (errorFlags & kContactorErrorMask) ? 1 : 0);
      out.push("bms_warn_low_soc", bit(warningFlags, 16));
      out.push("bms_warn_balancing_required", bit(warningFlags, 17));
      return;
    }

    case 0x202:
      if (data.size() < 8) return;
      out.push("allowed_discharge_a", u16be(data[4], data[5]) / 10.0);
      out.push("allowed_regen_a", u16be(data[6], data[7]) / 10.0);
      return;

    case 0x203: {
      if (data.size() < 8) return;
      const unsigned minCellMv = u16be(data[4], data[5]);
      const unsigned maxCellMv = u16be(data[6], data[7]);
      out.push("cell_avg_mv", u16be(data[0], data[1]));
      out.push("cell_min_mv", minCellMv);
      out.push("cell_max_mv", maxCellMv);
      out.push("cell_spread_mv", static_cast<double>(maxCellMv) - static_cast<double>(minCellMv));
      out.push("cell_lowest_v_idx", data[2]);
      out.push("cell_highest_v_idx", data[3]);
      return;
    }

    case 0x205:
      if (data.size() < 8) return;
      out.push("bms_remaining_energy_raw", u16be(data[0], data[1]));
      out.push("cell_deviation_mv", u16be(data[2], data[3]));
      out.push("remaining_ah", u16be(data[4], data[5]) / 10.0);
      out.push("cells_connected", u16be(data[6], data[7]));
      return;

    case 0x206:
      if (data.size() < 7) return;
      out.push("pack_resistance_mohm", u16be(data[0], data[1]));
      out.push("lmu_comm_warnings", u32be(data, 2));
      out.push("bms_io_state", data[6]);
      return;

    case 0x207:
      if (data.size() < 8) return;
      out.push("iso_test_1", u16le(data[0], data[1]));
      out.push("iso_test_2", u16le(data[2], data[3]));
      out.push("iso_test_total", u16le(data[4], data[5]));
      out.push("cell_voltage_sum_v", u16le(data[6], data[7]));
      return;

    case 0x300:
      if (data.size() < 7) return;
      out.push("charger_enabled", data[0]);
      out.push("bms_post_processor_1", u16le(data[1], data[2]));
      out.push("charger_max_dc_v", u16le(data[3], data[4]) / 10.0);
      out.push("charger_max_dc_a", u16le(data[5], data[6]) / 10.0);
      return;

    case 0x660:
      if (data.size() < 3) return;
      out.push("lmu_temp_high_idx", data[0]);
      out.push("lmu_temp_low_idx", data[1]);
      out.push("pack_temp_avg", signedByte(data[2]));
      if (data.size() >= kOffsetConfigMinDlc) {
        out.push("batt_temp_hi", signedByte(data[3]));
        out.push("batt_temp_lo", signedByte(data[4]));
      }
      if (data.size() >= kClampInstrumentationMinDlc) {
        out.push("clamp_gate", data[5]);
        out.push("clamp_amount", data[6]);
        out.push("batt_temp_hi_vcu_echo", signedByte(data[7]));
      }
      return;

    case 0x661:
      if (data.size() < 6) return;
      out.push("bms_remaining_energy_wh", u24be(data, 0));
      out.push("bms_uptime_min", u24be(data, 3));
      return;

    case 0x662:
      if (data.size() < 7) return;
      decodeLmuCellVoltages(data, {1, 2, 3}, out);
      return;

    case 0x663:
      if (data.size() < 7) return;
      decodeLmuCellVoltages(data, {4, 5, 6}, out);
      return;

    case 0x664:
      if (data.size() < 5) return;
      decodeLmuCellVoltages(data, {7, 8}, out);
      if (data.size() >= 8) {
        decodeLmuTemperatures(data, out);
      }
      return;

    case 0x665:
      if (data.size() < 8) return;
      out.push("cell_cutoff_mv", u16be(data[0], data[1]));
      out.push("cell_end_of_life_mv", u16be(data[2], data[3]));
      out.push("cell_overvoltage_mv", u16be(data[4], data[5]));
      out.push("cell_target_mv", u16be(data[6], data[7]));
      return;

    default:
      return;
  }
}

}  // namespace emcool::can
