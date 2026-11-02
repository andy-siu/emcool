#pragma once

// Whether the bike is safe to service, ported from cool-eva src/vcu/service-gate.ts and the
// evidence half of src/vcu/charge-session.ts. Pure: readings in, a verdict out.
//
// Every service-mode read and write asks this first, and the runners keep asking while they work.
// It wants the bike stopped, out of drive and not energized, each on a FRESH reading (a second for
// the broadcasts, ten for the OBD speed). Charging excuses "energized" (and the absence of the
// speed broadcasts), but only on evidence of a live charge — and a charge manager that reports an
// empty inlet vetoes that evidence.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "emcool/text/text_buffer.hpp"

namespace emcool::vcu {

struct GateSample {
  std::optional<double> value;
  std::optional<double> ageMs;
};

// The signals the gate looks at: its own rules' and the charge evidence's.
std::span<const std::string_view> serviceGateSignalKeys();
// The value and age of one of them, from live state.
using GateReader = std::function<GateSample(std::string_view key)>;

struct GateReadings {
  std::vector<std::pair<std::string_view, GateSample>> samples;
  const GateSample* find(std::string_view key) const;
};
GateReadings sampleServiceGate(const GateReader& read);

enum class GateCheckState : std::uint8_t { kOk, kUnsafe, kStale, kMissing, kExcusedByCharging, kInletEmpty };
const char* gateCheckStateName(GateCheckState state);

struct GateCheck {
  std::string_view key;
  std::string_view requirement;
  GateCheckState state;
  std::optional<double> value;
  std::optional<double> ageMs;
  bool required;
};

struct ServiceGateVerdict {
  bool safe = false;
  std::vector<std::string> blockers;
  std::vector<GateCheck> checks;
  std::optional<std::string> chargingEvidence;
};

ServiceGateVerdict evaluateServiceGate(const GateReadings& readings);

// The verdict as cool-eva's JSON.stringify writes it.
void writeVerdictJson(text::TextBuffer& out, const ServiceGateVerdict& verdict);

// --- Charge sessions (src/vcu/charge-session.ts) --------------------------------------------

inline constexpr double kChargeManagerStateAc = 0x02;
inline constexpr double kChargeManagerStateDc = 0x23;
inline constexpr double kChargeSessionMaxAgeMs = 5000;

// What says a charge is live, or nullopt.
std::optional<std::string> findChargingEvidence(const GateReadings& readings);
// The charge manager reports nothing in the inlet, on a fresh reading.
bool inletIsEmpty(const GateReadings& readings);
bool verdictSeesACable(const ServiceGateVerdict& verdict);

// The charge manager is on the bus, whatever it is saying (fresh charge_manager_state).
bool chargeManagerIsLive(std::optional<double> value, std::optional<double> ageMs);
// Is ANY charge path live — the question reset-vcu must ask before it runs.
bool chargePathIsActive(const GateReader& read);

// A live, settled charge session as the charge commands ask about it: present, fresh, AC or DC.
struct ChargeSession {
  double state;
  bool dc;  // true = DC (0x23), false = AC (0x02)
};
std::optional<ChargeSession> chargeSessionFrom(std::optional<double> value, std::optional<double> ageMs);

}  // namespace emcool::vcu
