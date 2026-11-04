#pragma once

// The per-frame decoders FrameDecoder routes to. Each appends to `out`; a frame too short or
// failing its gate appends nothing.

#include <cstdint>

#include "emcool/can/frame.hpp"
#include "emcool/can/frame_decoder.hpp"

namespace emcool::can {

inline constexpr std::uint32_t kAbsCanId = 0x0a0;
void decodeAbsFrame(FrameBytes data, DecodedValues& out);

void decodeAttitudeFrame(FrameBytes data, AxisRangeWatch& roll, AxisRangeWatch& pitch, DecodedValues& out);

// Two opcodes on one id.
inline constexpr std::uint32_t kChargeSetpointCanId = 0x121;
void decodeChargeSetpointFrame(FrameBytes data, DecodedValues& out);
void decodeChargeSocLimitFrame(FrameBytes data, DecodedValues& out);

bool isChargeManagerCanId(std::uint32_t id);
void decodeChargeManagerFrame(std::uint32_t id, FrameBytes data, DecodedValues& out);

inline constexpr std::uint32_t kClusterRangeCanId = 0x412;
void decodeClusterRangeFrame(FrameBytes data, DecodedValues& out);

inline constexpr std::uint32_t kConsumptionCanId = 0x10b;
void decodeConsumptionFrame(FrameBytes data, DecodedValues& out);

void decodeBmsFrame(std::uint32_t id, FrameBytes data, DecodedValues& out);

inline constexpr std::uint32_t kDriveTorqueCanId = 0x02c;
inline constexpr std::uint32_t kThrottleSensorCanId = 0x127;
inline constexpr std::uint32_t kRedundantSpeedCanId = 0x125;
void decodeDriveTorqueFrame(FrameBytes data, DecodedValues& out);
void decodeThrottleSensorFrame(FrameBytes data, DecodedValues& out);
void decodeRedundantSpeedFrame(FrameBytes data, DecodedValues& out);

// 0x410 goes to GpsMessageDecoder; decodeHubOutputFrame reads type 3.
inline constexpr std::uint32_t kGpsCanId = 0x410;
void decodeHubOutputFrame(FrameBytes data, DecodedValues& out);

inline constexpr std::uint32_t kPsuCanId = 0x501;
void decodePsuFrame(FrameBytes data, DecodedValues& out);

// 0x102's b0 switches and b3 flags.
void decodeHandlebarSwitches(std::uint8_t handlebar, DecodedValues& out);
void decodeVehicleFlagsByte3(std::uint8_t byte3, DecodedValues& out);

inline constexpr std::uint32_t kVcuFlagsCanId = 0x100;
void decodeVcuFlagsFrame(FrameBytes data, DecodedValues& out);

inline constexpr std::uint32_t kVehicleStatusCanId = 0x101;
void decodeVehicleStatusFrame(FrameBytes data, DecodedValues& out);

}  // namespace emcool::can
