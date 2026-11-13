#include "emcool/vcu/write_codec.hpp"

#include <cstdio>

#include "emcool/obd/obd_dtc.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/table_gate.hpp"

namespace emcool::vcu {

namespace {

constexpr std::uint8_t kSecurityAccess = 0x27;
constexpr std::uint8_t kWriteByCommonIdentifier = 0x2e;
constexpr std::uint8_t kStartRoutine = 0x31;
constexpr std::uint8_t kSecurityRequestSeed = 0x01;
constexpr std::uint8_t kSecuritySendKey = 0x02;
constexpr std::uint8_t kNegativeResponse = 0x7f;
constexpr std::uint8_t kPositiveOffset = 0x40;
constexpr std::uint32_t kSecurityKeyOffset = 0xc1a0babe;
constexpr std::size_t kMaxSingleFramePayload = 6;

std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}

bool isWriteService(std::uint8_t service) {
  return service == kSecurityAccess || service == kWriteByCommonIdentifier || service == kStartRoutine;
}

// The payload for a write-class request, or a reason it was refused.
struct PayloadResult {
  bool ok = false;
  std::vector<std::uint8_t> payload;
  std::string reason;
};

PayloadResult encodeWritePayload(const ParameterNames& names, const VcuWriteRequest& request) {
  PayloadResult result;
  switch (request.kind) {
    case VcuWriteRequest::Kind::kSecuritySeed:
      result.ok = true;
      result.payload = {kSecurityAccess, kSecurityRequestSeed};
      return result;
    case VcuWriteRequest::Kind::kSecurityKey: {
      const std::uint32_t key = request.key;
      result.ok = true;
      result.payload = {kSecurityAccess,
                        kSecuritySendKey,
                        static_cast<std::uint8_t>((key >> 24) & 0xff),
                        static_cast<std::uint8_t>((key >> 16) & 0xff),
                        static_cast<std::uint8_t>((key >> 8) & 0xff),
                        static_cast<std::uint8_t>(key & 0xff)};
      return result;
    }
    case VcuWriteRequest::Kind::kWriteParameter: {
      const ParameterWritePlan& plan = request.plan;
      if (!isAllowedPlan(names, plan)) {
        result.reason = "vcu-write: refusing to encode a write to identifier " + hex(plan.identifier) +
                        " — it is not on the allowlist in write_targets, or its bytes do not match what that module "
                        "would produce";
        return result;
      }
      const TableGateVerdict verdict =
          evaluateTableGate(request.tableTypePresent ? request.tableType : nullptr, names);
      if (!verdict.writesAllowed) {
        result.reason = "vcu-write: refusing to encode a parameter write — the VCU's parameter table is " +
                        std::string(tableGateStateName(verdict.state)) + ". " + verdict.reason + " " + verdict.remedy;
        return result;
      }
      if (const std::optional<std::string> problem =
              planTableProblem(names, plan, request.tableType, request.tableTypePresent)) {
        result.reason = "vcu-write: refusing to encode a write to " + plan.name + " — " + *problem;
        return result;
      }
      result.ok = true;
      result.payload = {kWriteByCommonIdentifier, static_cast<std::uint8_t>(plan.identifier >> 8),
                        static_cast<std::uint8_t>(plan.identifier & 0xff)};
      result.payload.insert(result.payload.end(), plan.record.begin(), plan.record.end());
      return result;
    }
    case VcuWriteRequest::Kind::kStartRoutine:
      result.ok = true;
      result.payload = {kStartRoutine, routineIdFor(request.routine)};
      return result;
  }
  result.reason = "vcu-write: unknown request kind";
  return result;
}

}  // namespace

std::uint8_t routineIdFor(ServiceRoutineName routine) {
  switch (routine) {
    case ServiceRoutineName::kSetServicePoint:
      return 0xfc;
  }
  return 0xfc;
}

Micro routineMicroFor(ServiceRoutineName routine) {
  switch (routine) {
    case ServiceRoutineName::kSetServicePoint:
      return Micro::kA8;
  }
  return Micro::kA8;
}

std::uint32_t securityKeyForSeed(std::uint32_t seed) {
  const std::uint32_t value = seed;
  const std::uint32_t swapped = ((value & 0xaaaaaaaau) >> 1) | ((value & 0x55555555u) << 1);
  // The addition in 64-bit then mod 2^32, matching the double-precision + >>> 0 of the TypeScript.
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(swapped) + kSecurityKeyOffset) % 0x100000000ull);
}

WriteFrameResult buildWriteFrame(const ParameterNames& names, Micro target, const VcuWriteRequest& request) {
  WriteFrameResult result;
  const PayloadResult payload = encodeWritePayload(names, request);
  if (!payload.ok) {
    result.reason = payload.reason;
    return result;
  }
  if (!isWriteService(payload.payload[0])) {
    result.reason = "vcu-write: refusing to transmit service " + hex(payload.payload[0]) + " — not a write-class service";
    return result;
  }
  if (payload.payload.size() > kMaxSingleFramePayload) {
    result.reason = "vcu-write: payload of " + std::to_string(payload.payload.size()) +
                    " bytes does not fit one frame — multi-frame is not built";
    return result;
  }
  result.frame[0] = targetAddress(target);
  result.frame[1] = static_cast<std::uint8_t>(payload.payload.size());
  for (std::size_t at = 0; at < payload.payload.size(); at++) result.frame[2 + at] = payload.payload[at];
  result.ok = true;
  return result;
}

SecurityAccessReply decodeSecurityAccessReply(std::span<const std::uint8_t> payload) {
  SecurityAccessReply reply;
  if (payload.empty()) {
    reply.reason = "empty payload";
    return reply;
  }
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    reply.kind = SecurityAccessReply::Kind::kRefused;
    reply.negativeResponseCode = payload[2];
    reply.description = obd::describeNegativeResponseCode(payload[2]);
    return reply;
  }
  if (payload[0] != kSecurityAccess + kPositiveOffset) {
    reply.reason = "reply names service " + hex(payload[0]) + ", not 0x67";
    return reply;
  }
  if (payload.size() < 2) {
    reply.reason = "positive reply without a sub-function";
    return reply;
  }
  if (payload[1] == kSecurityRequestSeed) {
    if (payload.size() < 6) {
      reply.reason = "seed reply carries " + std::to_string(payload.size() - 2) + " byte(s), not 4";
      return reply;
    }
    reply.kind = SecurityAccessReply::Kind::kSeed;
    reply.seed = (static_cast<std::uint32_t>(payload[2]) << 24) | (static_cast<std::uint32_t>(payload[3]) << 16) |
                 (static_cast<std::uint32_t>(payload[4]) << 8) | static_cast<std::uint32_t>(payload[5]);
    return reply;
  }
  if (payload[1] == kSecuritySendKey) {
    reply.kind = SecurityAccessReply::Kind::kUnlocked;
    if (payload.size() > 2) reply.trailing = payload[2];
    return reply;
  }
  reply.reason = "reply names sub-function " + hex(payload[1]) + ", not 01 or 02";
  return reply;
}

ParameterWriteReply decodeParameterWriteReply(std::span<const std::uint8_t> payload, int expectedIdentifier) {
  ParameterWriteReply reply;
  if (payload.empty()) {
    reply.reason = "empty payload";
    return reply;
  }
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    reply.kind = ParameterWriteReply::Kind::kRefused;
    reply.negativeResponseCode = payload[2];
    reply.description = obd::describeNegativeResponseCode(payload[2]);
    return reply;
  }
  if (payload[0] != kWriteByCommonIdentifier + kPositiveOffset) {
    reply.reason = "reply names service " + hex(payload[0]) + ", not 0x6E";
    return reply;
  }
  if (payload.size() < 3) {
    reply.reason = "positive reply without a full identifier echo";
    return reply;
  }
  const int received = (payload[1] << 8) | payload[2];
  if (received != expectedIdentifier) {
    reply.kind = ParameterWriteReply::Kind::kIdentifierMismatch;
    reply.expected = expectedIdentifier;
    reply.received = received;
    return reply;
  }
  reply.kind = ParameterWriteReply::Kind::kAccepted;
  reply.identifier = received;
  return reply;
}

RoutineReply decodeRoutineReply(std::span<const std::uint8_t> payload, ServiceRoutineName routine) {
  const int expected = routineIdFor(routine);
  RoutineReply reply;
  if (payload.empty()) {
    reply.reason = "empty payload";
    return reply;
  }
  if (payload[0] == kNegativeResponse) {
    if (payload.size() < 3) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    reply.kind = RoutineReply::Kind::kRefused;
    reply.negativeResponseCode = payload[2];
    reply.description = obd::describeNegativeResponseCode(payload[2]);
    return reply;
  }
  if (payload[0] != kStartRoutine + kPositiveOffset) {
    reply.reason = "reply names service " + hex(payload[0]) + ", not 0x71";
    return reply;
  }
  if (payload.size() < 2) {
    reply.reason = "positive reply without the routine id echoed back";
    return reply;
  }
  if (payload[1] != expected) {
    reply.kind = RoutineReply::Kind::kRoutineMismatch;
    reply.expected = expected;
    reply.received = payload[1];
    return reply;
  }
  reply.kind = RoutineReply::Kind::kStarted;
  reply.routine = payload[1];
  return reply;
}

}  // namespace emcool::vcu
