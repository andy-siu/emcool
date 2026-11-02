#include "emcool/can/frame_decoder.hpp"

#include "decoders.hpp"

// Ported from cool-eva src/can/decode.ts. The evidence behind every field — captures, match
// rates, refuted hypotheses — is in that file and in cool-eva docs/can-decode-findings.md;
// the one-line warnings kept here are the ones that stop a known mistake being re-made.

namespace emcool::can {

namespace {

void decodeEnergyStatusFrame(FrameBytes data, DecodedValues& out);
void decodeInverterTemperaturesFrame(FrameBytes data, DecodedValues& out);
void decodeSpeedOdometerFrame(FrameBytes data, DecodedValues& out);
void decodeThrottleAndLimitsFrame(FrameBytes data, DecodedValues& out);
void decodeBodyAndLightsFrame(FrameBytes data, AxisRangeWatch& roll, AxisRangeWatch& pitch,
                              DecodedValues& out);
void decodeDashboardInputsFrame(FrameBytes data, DecodedValues& out);

}  // namespace

void FrameDecoder::decode(std::uint32_t id, FrameBytes data, DecodedValues& out) {
  if (isChargeManagerCanId(id)) {
    decodeChargeManagerFrame(id, data, out);
    return;
  }
  switch (id) {
    case 0x10a:
      decodeEnergyStatusFrame(data, out);
      return;

    // INST.CONS: b0-1 LE ÷10 = Wh.
    case 0x025:
      if (data.size() < 2) return;
      out.push("inst_consumption_wh", u16le(data[0], data[1]) / 10.0);
      return;

    case kAbsCanId:
      decodeAbsFrame(data, out);
      return;
    case kDriveTorqueCanId:
      decodeDriveTorqueFrame(data, out);
      return;
    case kThrottleSensorCanId:
      decodeThrottleSensorFrame(data, out);
      return;
    case kRedundantSpeedCanId:
      decodeRedundantSpeedFrame(data, out);
      return;
    case kConsumptionCanId:
      decodeConsumptionFrame(data, out);
      return;
    case kPsuCanId:
      decodePsuFrame(data, out);
      return;

    // The dash's charge-current limit and SOC limit: two opcodes with different layouts.
    case kChargeSetpointCanId:
      decodeChargeSetpointFrame(data, out);
      decodeChargeSocLimitFrame(data, out);
      return;

    case kVcuFlagsCanId:
      decodeVcuFlagsFrame(data, out);
      return;
    case kVehicleStatusCanId:
      decodeVehicleStatusFrame(data, out);
      return;

    // Charger DC, charging only.
    case 0x305:
      if (data.size() < 7) return;
      out.push("mains_a", data[1] / 10.0);
      out.push("dc_a", u16le(data[3], data[4]) / 10.0);
      out.push("dc_v", u16le(data[5], data[6]) / 10.0);
      return;

    // Charger AC mains voltage, charging only.
    case 0x306:
      if (data.size() < 3) return;
      out.push("mains_v", data[2]);
      return;

    case 0x020:
      decodeInverterTemperaturesFrame(data, out);
      return;

    // Motor temperature: b4-5 LE s16 ÷10 °C. A different sensor from OBD PID 05.
    case 0x022:
      if (data.size() < 6) return;
      out.push("motor_temp_c", i16le(data[4], data[5]) / 10.0);
      return;

    case 0x104:
      decodeSpeedOdometerFrame(data, out);
      return;
    case 0x109:
      decodeThrottleAndLimitsFrame(data, out);
      return;
    case 0x102:
      decodeBodyAndLightsFrame(data, rollRangeWatch_, pitchRangeWatch_, out);
      return;

    // The cluster's hub message set: GPS (type 26) and the drive triple (type 3) share the
    // id, and a frame is one or the other, so both readers see every frame.
    case kGpsCanId:
      busGps_.decode(data, out);
      suppressedFixWatcher_.check();
      decodeHubOutputFrame(data, out);
      return;

    case kClusterRangeCanId:
      decodeClusterRangeFrame(data, out);
      return;

    // E-LOCK: b2-5 LE u32 = the key fob currently present.
    case 0x480:
      if (data.size() < 6) return;
      out.push("key_fob_id", u32le(data, 2));
      return;

    case 0x400:
      decodeDashboardInputsFrame(data, out);
      return;

    default:
      decodeBmsFrame(id, data, out);
      return;
  }
}

namespace {

// RES.ENERGY: b3-4 LE with bit 15 masked off (it is a flag that toggles on half the frames),
// ×2 = Wh. CHG.PWR.REF: b7 ÷ 7 = AC charge-current setpoint in amps.
void decodeEnergyStatusFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  out.push("residual_energy_wh", (u16le(data[3], data[4]) & 0x7fffu) * 2.0);
  out.push("charge_limit_a", jsRound((data[7] / 7.0) * 10.0) / 10.0);
}

// Four s16 LE ÷10 °C. Read signed, against the .xdbc, so a winter reading doesn't wrap.
void decodeInverterTemperaturesFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  out.push("inverter_igbt_min_c", i16le(data[0], data[1]) / 10.0);
  out.push("inverter_igbt_c", i16le(data[2], data[3]) / 10.0);
  out.push("inverter_igbt_max_c", i16le(data[4], data[5]) / 10.0);
  out.push("inverter_gate_c", i16le(data[6], data[7]) / 10.0);
}

// VCU_SPEEDODO. Speed is u15 at bit 32, rpm u15 at bit 47 ×4 counts. The bike's speed is
// geared driveline speed, +3.5 % against GPS: don't re-derive other scales against it.
void decodeSpeedOdometerFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 8) return;
  out.push("odometer_can_km", u32le(data, 0) / 10.0);
  out.push("speed_can_kmh", bitFieldLe(data, 32, 15) / 10.0);
  out.push("motor_rpm_can", bitFieldLe(data, 47, 15) * 4.0);
  out.push("odometer_pulse", bitFieldLe(data, 62, 1));
  out.push("rolling_backwards", bitFieldLe(data, 63, 1));
}

// Throttle plus the inverter's current limits. b6-7 is NOT a current (the .xdbc's
// `current_other_a` read 5069 A); only b6's two event bits are decoded.
void decodeThrottleAndLimitsFrame(FrameBytes data, DecodedValues& out) {
  // Throttle keeps its 2-byte guard so a short frame can't silence it.
  if (data.size() < 2) return;
  out.push("throttle_pct", u16le(data[0], data[1]) / 10.0);
  if (data.size() >= 8) {
    out.push("current_max_out_a", u16le(data[2], data[3]) / 10.0);
    out.push("current_max_regen_a", u16le(data[4], data[5]) / 10.0);
    out.push("eabs_event", bit(data[6], 6));
    out.push("tc_event", bit(data[6], 7));
  }
}

// Body, lights, vehicle state and attitude. b2 bits 0/1 are the BEAM LAMPS — they were once
// `charging`/`charge_port_unlocked` off the .xdbc; do not restore those. `throttle_on` (b1
// bit 7) is NOT inverted despite the vendor's `THROTTLE_CLOSED_SW` name: the VCU write gate
// relies on the measured polarity.
void decodeBodyAndLightsFrame(FrameBytes data, AxisRangeWatch& roll, AxisRangeWatch& pitch,
                              DecodedValues& out) {
  if (data.size() < 3) return;
  const std::uint8_t handlebar = data[0];
  const std::uint8_t vehicleState = data[1];
  const std::uint8_t lampsAndState = data[2];
  out.push("high_beam", (handlebar & 0x40) ? 1 : 0);
  // Front and rear are independent circuits; neither implies the other.
  out.push("front_brake", bit(lampsAndState, 5));
  out.push("rear_brake", bit(lampsAndState, 6));
  out.push("blinker_left", (lampsAndState & 0x04) ? 1 : 0);
  out.push("blinker_right", (lampsAndState & 0x08) ? 1 : 0);
  out.push("horn", (lampsAndState & 0x10) ? 1 : 0);
  out.push("horn_switch", bit(vehicleState, 0));
  out.push("energized", bit(vehicleState, 1));
  out.push("go_request", bit(vehicleState, 2));
  out.push("go", bit(vehicleState, 3));
  out.push("key_on", bit(vehicleState, 4));
  out.push("stand_up", bit(vehicleState, 5));
  out.push("ignition_button", bit(vehicleState, 6));
  out.push("throttle_on", bit(vehicleState, 7));
  out.push("high_beam_lamp", bit(lampsAndState, 0));
  out.push("low_beam_lamp", bit(lampsAndState, 1));
  out.push("moving", bit(lampsAndState, 7));
  decodeHandlebarSwitches(handlebar, out);
  // b3 has its own guard so a short frame can't silence the b0-2 signals.
  if (data.size() >= 4) {
    decodeVehicleFlagsByte3(data[3], out);
  }
  decodeAttitudeFrame(data, roll, pitch, out);
}

// The dashboard's own digital inputs: b2 is the button bitfield, b5 bit 7 the day/night flag.
// Bit names are Energica's IO table; what each is measured to do is in cool-eva decode.ts.
void decodeDashboardInputsFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 3) return;
  const std::uint8_t buttons = data[2];
  out.push("btn_set_back", bit(buttons, 0));
  // Not side-effect free: even a sub-second press arms cruise.
  out.push("btn_cruise_enable", bit(buttons, 1));
  out.push("btn_cruise_set", bit(buttons, 2));
  out.push("btn_heated_grip", bit(buttons, 3));
  if (data.size() >= 6) {
    out.push("dash_day_mode", bit(data[5], 7));
  }
}

}  // namespace

}  // namespace emcool::can
