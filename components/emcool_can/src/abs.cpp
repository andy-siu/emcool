#include "decoders.hpp"

namespace emcool::can {

namespace {

// Within ~1 % of GPS. The residual is the tyres, so the GPS-fitted pair is deliberately not
// used: it would drift with tyre wear and break front/rear comparability.
constexpr double kWheelSpeedKmhPerCount = 0.05625;

}  // namespace

void decodeAbsFrame(FrameBytes data, DecodedValues& out) {
  if (data.size() < 6) return;
  out.push("wheel_speed_front_kmh", u16le(data[0], data[1]) * kWheelSpeedKmhPerCount);
  out.push("wheel_speed_rear_kmh", u16le(data[2], data[3]) * kWheelSpeedKmhPerCount);
  out.push("abs_warning_lamp", (data[4] & 0x0c) >> 2);
  out.push("abs_front_sensor_fault", bit(data[4], 4));
  out.push("abs_rear_sensor_fault", bit(data[4], 5));
  out.push("abs_event", bit(data[4], 7));
  // Front-specific by measurement, settled by 0x102's rear-only brake applications.
  out.push("front_brake_pressure_bar", data[5]);
  if (data.size() >= 7) {
    out.push("abs_front_pressure_validity", bit(data[6], 0));
    out.push("abs_front_control_active", bit(data[6], 1));
    out.push("abs_rear_control_active", bit(data[6], 2));
  }
}

}  // namespace emcool::can
