#pragma once

// Pure codec for the three services that CHANGE something in a VCU micro, ported from cool-eva
// src/vcu/write-codec.ts: SecurityAccess (0x27), WriteDataByCommonIdentifier (0x2E) and
// StartRoutineByLocalIdentifier (0x31). Requests in, bytes out; bytes in, outcomes out. No bus.
// codec.hpp's read-only guarantee is untouched — its request union still has nowhere to put a value.
//
// Four things a caller cannot do, each re-checked on the way out (cool-eva docs/vcu-parameters.md §6):
//   - name an IDENTIFIER (write-parameter takes a PLAN; the allowlist in write_targets makes one,
//     and isAllowedPlan re-derives it here)
//   - name a ROUTINE ID (start-routine takes a NAME; 0xFB, the routine that WIPES BATTERY
//     STATISTICS, has no name here and so is unreachable)
//   - write before the bike has named its parameter table (assertTableTypeConfirmed)
//   - emit a service outside WRITE_SERVICES
//
// cool-eva throws on the guards (they only fire on misuse, never in normal operation); emcool runs
// with -fno-exceptions, so buildWriteFrame returns a failed result with the same reason instead.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/vcu/snapshot.hpp"
#include "emcool/vcu/tables.hpp"
#include "emcool/vcu/write_targets.hpp"

namespace emcool::vcu {

// The routines this repo will start, by name. One member; 0xFB/0xFA have no name here, on purpose.
enum class ServiceRoutineName : std::uint8_t { kSetServicePoint };

// Everything this codebase is permitted to ask a VCU micro to DO. Closed on purpose.
struct VcuWriteRequest {
  enum class Kind : std::uint8_t { kSecuritySeed, kSecurityKey, kWriteParameter, kStartRoutine };
  Kind kind;
  std::uint32_t key = 0;                            // kSecurityKey
  ParameterWritePlan plan;                          // kWriteParameter
  const TableTypeReport* tableType = nullptr;       // kWriteParameter (nullopt-equivalent: nullptr)
  bool tableTypePresent = false;                    // whether a report was supplied at all
  ServiceRoutineName routine = ServiceRoutineName::kSetServicePoint;  // kStartRoutine
};

// A8 or A9, by name, with the routine and the micro that owns it.
std::uint8_t routineIdFor(ServiceRoutineName routine);
Micro routineMicroFor(ServiceRoutineName routine);

// swap each adjacent bit pair of the 32-bit seed, then + 0xC1A0BABE mod 2^32.
std::uint32_t securityKeyForSeed(std::uint32_t seed);

struct WriteFrameResult {
  bool ok = false;
  std::array<std::uint8_t, 8> frame{};
  std::string reason;  // when !ok (the refusal cool-eva would have thrown)
};
WriteFrameResult buildWriteFrame(const ParameterNames& names, Micro target, const VcuWriteRequest& request);

// --- SecurityAccess reply -------------------------------------------------------------------
struct SecurityAccessReply {
  enum class Kind : std::uint8_t { kSeed, kUnlocked, kRefused, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  std::uint32_t seed = 0;                 // kSeed
  std::optional<int> trailing;            // kUnlocked
  int negativeResponseCode = 0;           // kRefused
  std::string description;                // kRefused
  std::string reason;                     // kUnrecognised
};
SecurityAccessReply decodeSecurityAccessReply(std::span<const std::uint8_t> payload);

// --- WriteDataByCommonIdentifier reply ------------------------------------------------------
struct ParameterWriteReply {
  enum class Kind : std::uint8_t { kAccepted, kIdentifierMismatch, kRefused, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  int identifier = 0;                     // kAccepted
  int expected = 0;                       // kIdentifierMismatch
  int received = 0;                       // kIdentifierMismatch
  int negativeResponseCode = 0;           // kRefused
  std::string description;                // kRefused
  std::string reason;                     // kUnrecognised
};
ParameterWriteReply decodeParameterWriteReply(std::span<const std::uint8_t> payload, int expectedIdentifier);

// --- StartRoutine reply ---------------------------------------------------------------------
struct RoutineReply {
  enum class Kind : std::uint8_t { kStarted, kRoutineMismatch, kRefused, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  int routine = 0;                        // kStarted
  int expected = 0;                       // kRoutineMismatch
  int received = 0;                       // kRoutineMismatch
  int negativeResponseCode = 0;           // kRefused
  std::string description;                // kRefused
  std::string reason;                     // kUnrecognised
};
RoutineReply decodeRoutineReply(std::span<const std::uint8_t> payload, ServiceRoutineName routine);

// Re-derives a plan from the allowlist and says whether it matches. Defined in write_targets.cpp,
// called here immediately before a 2E frame is built — the codec's "cannot write a
// non-allowlisted identifier" made true of the CODEC, not only of today's callers.
bool isAllowedPlan(const ParameterNames& names, const ParameterWritePlan& plan);
// Why writing this plan on the bike the report names is unsafe, or nullopt. Defined in
// write_targets.cpp; the last point a misrouted write can be stopped.
std::optional<std::string> planTableProblem(const ParameterNames& names, const ParameterWritePlan& plan,
                                             const TableTypeReport* report, bool reportPresent);

}  // namespace emcool::vcu
