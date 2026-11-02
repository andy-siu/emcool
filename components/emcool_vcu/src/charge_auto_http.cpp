#include "emcool/vcu/charge_auto_http.hpp"

namespace emcool::vcu {

// cool-eva CHARGE_AUTO_REASON_TEXT, with TARGET_C (54) and MIN_COMMAND_A (35) substituted in.
const char* chargeAutoReasonText(ChargeAutoReason reason) {
  switch (reason) {
    case ChargeAutoReason::kDisabled:
      return "Off — the bike charges as it normally would.";
    case ChargeAutoReason::kNotDc:
      return "Waiting for a DC fast charge.";
    case ChargeAutoReason::kNoTemperature:
      return "No trustworthy pack temperature — not commanding anything.";
    case ChargeAutoReason::kNoCeiling:
      return "The DC ceiling has not arrived, so there is nothing to command against.";
    case ChargeAutoReason::kRider:
      return "You set a different current — stood down for this charge.";
    case ChargeAutoReason::kNoHistory:
      return "Watching. Not enough temperature history yet to see a trend.";
    case ChargeAutoReason::kBlindDescent:
      return "Arrived hot with no trend yet — easing the current down.";
    case ChargeAutoReason::kHardCeiling:
      return "At 54 °C or above — easing the current down.";
    case ChargeAutoReason::kClosing:
      return "Warming towards 54 °C too fast to catch — easing the current down.";
    case ChargeAutoReason::kClear:
      return "Below 54 °C with room to spare — giving current back, a step at a time.";
    case ChargeAutoReason::kSettled:
      return "Holding — this current is taking the pack where it should be.";
    case ChargeAutoReason::kNearCeiling:
      return "At 54 °C and not warming — holding this current.";
    case ChargeAutoReason::kAtFloor:
      return "At the 35 A floor — going lower would be slower than not acting.";
    case ChargeAutoReason::kTapering:
      return "The pack's own taper takes the current away before it could reach 54 °C — holding, rather than taking "
             "it away first.";
    case ChargeAutoReason::kMeasuring:
      return "Holding the last change until the pack has answered it.";
  }
  return "";
}

void writeChargeAutoResponseJson(text::TextBuffer& out, const ChargeAutoState& state,
                                 const std::optional<std::string>& message) {
  out.append("{\"state\":{\"mode\":");
  out.appendJsonString(state.mode == ChargeAutoMode::kAutomatic ? "automatic" : "off");
  out.append(",\"reason\":");
  out.appendInteger(static_cast<int>(state.reason));
  out.append(",\"commandedAmps\":");
  if (state.commandedAmps) {
    out.appendInteger(*state.commandedAmps);
  } else {
    out.append("null");
  }
  out.append("},\"reasonText\":");
  out.appendJsonString(chargeAutoReasonText(state.reason));
  out.append(",\"floorAmps\":");
  out.appendInteger(kMinCommandA);
  out.append(",\"message\":");
  if (message) {
    out.appendJsonString(*message);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace emcool::vcu
