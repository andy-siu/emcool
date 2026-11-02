#include "emcool/vcu/charge_command.hpp"

namespace emcool::vcu {

std::optional<std::array<std::uint8_t, 8>> buildChargeSocLimitWrite(int percent) {
  if (percent < 0 || percent > kMaxSocLimitPct) return std::nullopt;
  std::array<std::uint8_t, 8> frame{};
  frame[0] = static_cast<std::uint8_t>(kChargeSocLimitOpcode | kRequestTwinBit);
  frame[1] = kSeparatorByte;
  frame[2] = static_cast<std::uint8_t>(percent);
  return frame;
}

std::array<std::uint8_t, 8> buildChargeSocLimitRead() {
  std::array<std::uint8_t, 8> frame{};
  frame[0] = kChargeSocLimitOpcode;
  frame[1] = kSeparatorByte;
  return frame;
}

namespace {
constexpr std::uint8_t kDcCurrentLimitOpcode = 0x18;
constexpr std::uint8_t kAcCurrentLimitOpcode = 0x1a;
constexpr std::uint8_t kLimitInForce = 1;
constexpr std::uint8_t kStopOpcode = 0x16;
constexpr std::uint8_t kStopArgByte = 1;
}  // namespace

const char* chargeModeName(ChargeMode mode) { return mode == ChargeMode::kDc ? "dc" : "ac"; }
const char* chargeModeNameUpper(ChargeMode mode) { return mode == ChargeMode::kDc ? "DC" : "AC"; }

std::vector<ChargeFrame> buildChargeCurrentCommand(ChargeMode mode, int selectedAmps, int ceilingAmps,
                                                   std::string& error) {
  if (ceilingAmps < 1 || ceilingAmps > 255) {
    error = "charge-command: ceiling " + std::to_string(ceilingAmps) +
            " A is outside the 1…255 this frame's byte can hold";
    return {};
  }
  if (selectedAmps < 1 || selectedAmps > ceilingAmps) {
    error = "charge-command: " + std::to_string(selectedAmps) + " A must be between 1 and the ceiling " +
            std::to_string(ceilingAmps) + " A";
    return {};
  }
  const std::uint8_t opcode = mode == ChargeMode::kDc ? kDcCurrentLimitOpcode : kAcCurrentLimitOpcode;
  std::array<std::uint8_t, 8> commit{};
  commit[0] = static_cast<std::uint8_t>(opcode | kRequestTwinBit);
  commit[1] = kSeparatorByte;
  commit[2] = static_cast<std::uint8_t>(selectedAmps);
  std::array<std::uint8_t, 8> command{};
  command[0] = opcode;
  command[1] = kSeparatorByte;
  command[2] = static_cast<std::uint8_t>(selectedAmps);
  command[3] = kLimitInForce;
  command[4] = static_cast<std::uint8_t>(ceilingAmps);
  return {{kChargeRequestCanId, commit}, {kChargeCommandCanId, command}};
}

std::vector<ChargeFrame> buildChargeStopCommand() {
  std::array<std::uint8_t, 8> request{};
  request[0] = static_cast<std::uint8_t>(kStopOpcode | kRequestTwinBit);
  request[1] = kSeparatorByte;
  request[2] = kStopArgByte;
  return {{kChargeRequestCanId, request}};
}

}  // namespace emcool::vcu
