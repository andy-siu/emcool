#pragma once

// The per-frame decoders FrameDecoder routes to, one per cool-eva module (the name after
// each group). Each appends to `out`; a frame too short or failing its gate appends nothing.

#include <cstdint>

#include "emcool/can/frame.hpp"
#include "emcool/can/frame_decoder.hpp"

namespace emcool::can {

// src/can/abs.ts
inline constexpr std::uint32_t kAbsCanId = 0x0a0;
void decodeAbsFrame(FrameBytes data, DecodedValues& out);

// src/can/attitude.ts
void decodeAttitudeFrame(FrameBytes data, AxisRangeWatch& roll, AxisRangeWatch& pitch, DecodedValues& out);

// src/can/charge-setpoint.ts and src/can/charge-soc-limit.ts — two opcodes on one id.
inline constexpr std::uint32_t kChargeSetpointCanId = 0x121;
void decodeChargeSetpointFrame(FrameBytes data, DecodedValues& out);
void decodeChargeSocLimitFrame(FrameBytes data, DecodedValues& out);

// src/can/charge-manager.ts
bool isChargeManagerCanId(std::uint32_t id);
void decodeChargeManagerFrame(std::uint32_t id, FrameBytes data, DecodedValues& out);

// src/can/cluster-range.ts
inline constexpr std::uint32_t kClusterRangeCanId = 0x412;
void decodeClusterRangeFrame(FrameBytes data, DecodedValues& out);

// src/can/consumption.ts
inline constexpr std::uint32_t kConsumptionCanId = 0x10b;
void decodeConsumptionFrame(FrameBytes data, DecodedValues& out);

// src/can/decode-bms.ts
void decodeBmsFrame(std::uint32_t id, FrameBytes data, DecodedValues& out);

// src/can/drive.ts
inline constexpr std::uint32_t kDriveTorqueCanId = 0x02c;
inline constexpr std::uint32_t kThrottleSensorCanId = 0x127;
inline constexpr std::uint32_t kRedundantSpeedCanId = 0x125;
void decodeDriveTorqueFrame(FrameBytes data, DecodedValues& out);
void decodeThrottleSensorFrame(FrameBytes data, DecodedValues& out);
void decodeRedundantSpeedFrame(FrameBytes data, DecodedValues& out);

// src/can/gps.ts routes 0x410 to GpsMessageDecoder; src/can/hub-output.ts reads type 3.
inline constexpr std::uint32_t kGpsCanId = 0x410;
void decodeHubOutputFrame(FrameBytes data, DecodedValues& out);

// src/can/psu.ts
inline constexpr std::uint32_t kPsuCanId = 0x501;
void decodePsuFrame(FrameBytes data, DecodedValues& out);

// src/can/vcu-digitals.ts — 0x102's b0 switches and b3 flags.
void decodeHandlebarSwitches(std::uint8_t handlebar, DecodedValues& out);
void decodeVehicleFlagsByte3(std::uint8_t byte3, DecodedValues& out);

// src/can/vcu-flags.ts
inline constexpr std::uint32_t kVcuFlagsCanId = 0x100;
void decodeVcuFlagsFrame(FrameBytes data, DecodedValues& out);

// src/can/vehicle-status.ts
inline constexpr std::uint32_t kVehicleStatusCanId = 0x101;
void decodeVehicleStatusFrame(FrameBytes data, DecodedValues& out);

}  // namespace emcool::can
