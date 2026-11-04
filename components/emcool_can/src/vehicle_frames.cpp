#include "decoders.hpp"

// The small vehicle-frame decoders.

namespace emcool::can {

namespace {

// The VCU clamps at 65000, not 0xFFFF. The two fields of a pair are reciprocals of one
// quantity, so either one at either end (0 or 65000) leaves the whole pair undefined.
constexpr unsigned kSaturatedHigh = 65000;

bool isSaturated(unsigned raw) { return raw == 0 || raw >= kSaturatedHigh; }

}  // namespace

void decodeClusterRangeFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) {
    return;
  }
  out.push("range_can_km", u16le(data[2], data[3]));
}

// Each pair is withheld together if either half is saturated.
void decodeConsumptionFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  const unsigned instantKmPerKwh = u16le(data[0], data[1]);
  const unsigned instantKwhPer100Km = u16le(data[2], data[3]);
  if (!isSaturated(instantKmPerKwh) && !isSaturated(instantKwhPer100Km)) {
    out.push("km_per_kwh_can", instantKmPerKwh / 10.0);
    out.push("kwh_per_100km_can", instantKwhPer100Km / 1000.0);
  }
  const unsigned average100mKmPerKwh = u16le(data[4], data[5]);
  const unsigned average100mKwhPer100Km = u16le(data[6], data[7]);
  if (!isSaturated(average100mKmPerKwh) && !isSaturated(average100mKwhPer100Km)) {
    out.push("km_per_kwh_100m_can", average100mKmPerKwh / 10.0);
    out.push("kwh_per_100km_100m_can", average100mKwhPer100Km / 1000.0);
  }
}

void decodeDriveTorqueFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 4) return;
  out.push("drive_torque_cmd_nm", i16le(data[0], data[1]) / 10.0);
  out.push("drive_torque_feedback_nm", i16le(data[2], data[3]) / 10.0);
}

void decodeThrottleSensorFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 4) return;
  out.push("throttle_sensor_a_raw", u16le(data[0], data[1]));
  out.push("throttle_sensor_b_raw", u16le(data[2], data[3]));
}

void decodeRedundantSpeedFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 4) return;
  out.push("speed_redundant_a_raw", u16le(data[0], data[1]));
  out.push("speed_redundant_b_raw", u16le(data[2], data[3]));
}

void decodePsuFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 6) return;
  out.push("psu_12v_mv", u16le(data[0], data[1]));
  out.push("psu_12v_lowpower_mv", u16le(data[2], data[3]));
  out.push("psu_12v_load_ma", u16le(data[4], data[5]));
}

// 0x102 b0, the handlebar SWITCHES (b2 carries the lamp outputs).
void decodeHandlebarSwitches(std::uint8_t handlebar, DecodedValues& out) {
  out.push("btn_mode_left", bit(handlebar, 0));
  out.push("btn_mode_right", bit(handlebar, 1));
  out.push("btn_mode_enter", bit(handlebar, 2));
  out.push("blinker_switch_right", bit(handlebar, 3));
  out.push("blinker_switch_left", bit(handlebar, 4));
  out.push("low_beam_switch", bit(handlebar, 7));
  out.push("btn_indicator_cancel", bit(handlebar, 5));
}

// 0x102 b3.
void decodeVehicleFlagsByte3(std::uint8_t byte3, DecodedValues& out) {
  out.push("fast_dc_contactor", bit(byte3, 0));
  out.push("cruise_active", bit(byte3, 1));
  out.push("dsb_control", bit(byte3, 2));
  out.push("imd_disable", bit(byte3, 3));
  out.push("winter_storage", bit(byte3, 4));
  out.push("vcu_abs_off", bit(byte3, 7));
  out.push("lie_down_detected", bit(byte3, 5));
  out.push("mag_good", bit(byte3, 6));
}

void decodeVcuFlagsFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  out.push("vcu_flags_low", u32le(data, 0));
  out.push("vcu_flags_high", u32le(data, 4));
  out.push("vcu_err_charge_manager", bit(data[7], 1));
  out.push("vcu_err_check_modules", bit(data[2], 7));
  out.push("vcu_check_modules_status", bit(data[4], 3));
  out.push("vcu_warn_soc_misaligned", bit(data[6], 4));
  out.push("vcu_12v_power_good", bit(data[7], 0));
  out.push("vcu_err_system_fault", bit(data[0], 0));
  out.push("vcu_err_battery_ot", bit(data[3], 4));
  out.push("vcu_err_motor_ot", bit(data[3], 5));
  out.push("vcu_err_system_fatal_fault", bit(data[3], 6));
  out.push("vcu_err_system_blocking_fault", bit(data[3], 7));
  out.push("vcu_err_drive_ot", bit(data[4], 1));
  out.push("vcu_err_leak_detect", bit(data[6], 0));
}

// The VCU's own state machine. The same two state words also arrive over BLE
// as `vehicle_state`/`vehicle_substate`; these carry `_can`.
void decodeVehicleStatusFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  const std::uint8_t flags = data[3];
  out.push("vehicle_substate_can", data[0]);
  out.push("vehicle_state_can", data[1]);
  out.push("drive_vsm", data[2]);
  out.push("drive_vsm_b3", flags & 0x03);
  out.push("limp_mode_status", bit(flags, 2));
  out.push("limp_res_valid", bit(flags, 3));
  out.push("vehicle_status_flags", flags);
  out.push("limp_pack_res", u16le(data[4], data[5]));
  out.push("limp_module_word", u16le(data[6], data[7]));
}

}  // namespace emcool::can
