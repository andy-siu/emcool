#pragma once

// The /charge-auto response body: {state, reasonText, floorAmps, message}. The reason text is the
// TypeScript reference's, verbatim.

#include <optional>
#include <string>

#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/charge_auto.hpp"

namespace emcool::vcu {

inline constexpr const char* kChargeAutoHeaderValue = "charge-auto";

const char* chargeAutoReasonText(ChargeAutoReason reason);
void writeChargeAutoResponseJson(text::TextBuffer& out, const ChargeAutoState& state,
                                 const std::optional<std::string>& message);

}  // namespace emcool::vcu
