#include <cstdio>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"
#include "emcool/vcu/charge_command.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/service_gate.hpp"
#include "emcool/vcu/write_runner.hpp"

// The charge-current command (0x121) and the charge-stop (0x120). Both are fire-and-forget event
// frames with no reply, so "sent" is the strongest claim; the ack-watch reads the bike's answer over
// the next seconds.

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr std::int64_t kCurrentFrameGapMs = 5;
constexpr std::int64_t kStopFrameGapMs = 20;
// The AC ceiling (0x121 b4) to use when the dash has not broadcast ac_charge_ceiling_a this
// session — the remote case. 15 is the only AC ceiling ever captured; the live signal wins.
constexpr int kAcCeilingFallbackA = 15;

std::string describeNoSession(std::optional<double> state, std::optional<double> ageMs, const std::string& what) {
  if (!chargeManagerIsLive(state, ageMs)) {
    return "not charging — charge_manager_state is absent or stale, so there is no live session to " + what +
           ". Plug the bike in first.";
  }
  char stateHex[16];
  std::snprintf(stateHex, sizeof stateHex, "0x%x", state ? static_cast<int>(*state) : 0);
  return std::string("charge_manager_state reads ") + stateHex +
         ", not a settled AC (0x02) or DC (0x23) session — the charge handshake may still be in progress. Retry in a "
         "moment.";
}

}  // namespace

async::Task<ServiceWriteAnswer> VcuWriteRunner::performChargeCurrent(const ServiceWriteRequest& request) {
  const GateSample stateSample = context_.readSignal("charge_manager_state");
  const std::optional<ChargeSession> session = chargeSessionFrom(stateSample.value, stateSample.ageMs);
  if (!session) {
    co_return ServiceWriteAnswer{false, {}, describeNoSession(stateSample.value, stateSample.ageMs, "command a current into")};
  }
  const ChargeMode mode = session->dc ? ChargeMode::kDc : ChargeMode::kAc;
  const char* ceilingKey = session->dc ? "fast_dc_limit_max_a" : "ac_charge_ceiling_a";
  const GateSample ceilingSample = context_.readSignal(ceilingKey);
  std::optional<double> ceiling = ceilingSample.value;
  if (!ceiling && mode == ChargeMode::kAc) {
    logWarning(kTag,
               "AC charge ceiling (%s) not seen this session — defaulting b4 to %d A. If this charger's rating "
               "differs, the VCU will settle on ~10 A.",
               ceilingKey, kAcCeilingFallbackA);
    ceiling = kAcCeilingFallbackA;
  }
  if (!ceiling) {
    co_return ServiceWriteAnswer{
        false, {},
        "the DC charge ceiling (fast_dc_limit_max_a) has not arrived — it broadcasts whenever the bike is awake, so "
        "this means CAN is not being received. Not commanding blind."};
  }
  const int ceilingAmps = static_cast<int>(*ceiling);
  if (request.amps < 1 || request.amps > ceilingAmps) {
    co_return ServiceWriteAnswer{false, {},
                                 std::to_string(request.amps) + " A must be a whole number between 1 and the live " +
                                     chargeModeNameUpper(mode) + " ceiling of " + std::to_string(ceilingAmps) + " A"};
  }

  logWarning(kTag, "about to command %s charge current %d A (ceiling %d A) on 0x121", chargeModeNameUpper(mode),
             request.amps, ceilingAmps);
  // ⚠️ BEFORE the send: the bike answers our 0x120 commit with a 0x121 carrying these amps within
  // milliseconds, so a controller told afterwards reads its own command as the rider.
  if (context_.onChargeCurrentOutgoing) context_.onChargeCurrentOutgoing(request.amps, request.origin);

  std::string error;
  const std::vector<ChargeFrame> frames = buildChargeCurrentCommand(mode, request.amps, ceilingAmps, error);
  std::string sentHex;
  std::optional<std::string> sendFailure = error.empty() ? std::nullopt : std::optional<std::string>(error);
  if (!sendFailure) {
    for (std::size_t at = 0; at < frames.size(); at++) {
      if (const std::optional<std::string> problem = context_.send(frames[at].id, frames[at].data)) {
        sendFailure = *problem;
        break;
      }
      char id[12];
      std::snprintf(id, sizeof id, "0x%x", static_cast<unsigned>(frames[at].id));
      if (!sentHex.empty()) sentHex += " / ";
      sentHex += std::string(id) + " " + toHex(frames[at].data);
      if (at + 1 < frames.size()) co_await async::sleep(kCurrentFrameGapMs);
    }
  }
  if (!sendFailure) {
    logWarning(kTag, "sent %s charge current %d A (ceiling %d A) — %s", chargeModeNameUpper(mode), request.amps,
               ceilingAmps, sentHex.c_str());
    // Starts the acknowledgement window, after the send, so a frame that never left is not watched.
    if (context_.ackWatch != nullptr && context_.monotonicMs) {
      context_.ackWatch->noteSent(mode, request.amps, context_.monotonicMs());
    }
  }

  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kChargeCurrent;
  record.status = sendFailure ? "failed" : "sent";
  record.requested = AuditValue::of(static_cast<double>(request.amps));
  record.after = AuditValue::null();
  if (!sendFailure) record.rawHex = sentHex;
  record.note = sendFailure ? *sendFailure
                            : std::string(chargeModeNameUpper(mode)) + " " + std::to_string(request.amps) +
                                  " A, ceiling " + std::to_string(ceilingAmps) +
                                  " A, on 0x121; event frame with no reply — confirm on the dash / charge_limit_a";
  audit(std::move(record));

  // Even a failure on the first frame counts as possibly sent, as the reference's does: the 0x120 commit
  // twin goes first, and a send error does not prove nothing reached the bus.
  if (sendFailure) co_return ServiceWriteAnswer{false, {}, *sendFailure, true};
  ServiceWriteResult result;
  result.action = AuditAction::kChargeCurrent;
  result.status = "sent";
  result.message =
      "Commanded " + std::string(chargeModeNameUpper(mode)) + " charge current " + std::to_string(request.amps) +
      " A (" + sentHex +
      "). ⚠️ This is an event frame with no reply — watch the dash's set value and charge_limit_a to see it take. A "
      "full battery caps the current that actually flows regardless. The setting is transient (unplugging resets it) "
      "and you can override it on the bike's own screen.";
  result.succeeded = true;
  co_return ServiceWriteAnswer{true, result, {}};
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performChargeStop() {
  const GateSample stateSample = context_.readSignal("charge_manager_state");
  const std::optional<ChargeSession> session = chargeSessionFrom(stateSample.value, stateSample.ageMs);
  if (!session) {
    co_return ServiceWriteAnswer{false, {}, describeNoSession(stateSample.value, stateSample.ageMs, "stop")};
  }
  const char* mode = session->dc ? "DC" : "AC";
  logWarning(kTag, "about to stop the %s charge — injecting the 0x120 Mode-stop request-twin", mode);
  const std::vector<ChargeFrame> frames = buildChargeStopCommand();
  std::string sentHex;
  std::optional<std::string> sendFailure;
  for (std::size_t at = 0; at < frames.size(); at++) {
    if (const std::optional<std::string> problem = context_.send(frames[at].id, frames[at].data)) {
      sendFailure = *problem;
      break;
    }
    char id[12];
    std::snprintf(id, sizeof id, "0x%x", static_cast<unsigned>(frames[at].id));
    if (!sentHex.empty()) sentHex += " / ";
    sentHex += std::string(id) + " " + toHex(frames[at].data);
    if (at + 1 < frames.size()) co_await async::sleep(kStopFrameGapMs);
  }

  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kChargeStop;
  record.status = sendFailure ? "failed" : "sent";
  record.after = AuditValue::null();
  if (!sendFailure) record.rawHex = sentHex;
  record.note = sendFailure ? *sendFailure
                            : std::string(mode) + " stop on 0x120; event frame with no reply — confirm by the charge "
                                                  "winding down";
  audit(std::move(record));

  if (sendFailure) co_return ServiceWriteAnswer{false, {}, *sendFailure};
  logWarning(kTag, "sent the charge-stop command — %s", sentHex.c_str());
  ServiceWriteResult result;
  result.action = AuditAction::kChargeStop;
  result.status = "sent";
  result.message =
      "Sent the stop-charging command (" + sentHex +
      "). ⚠️ These are event frames with no reply — the charge winds down over the next several seconds as the bike's "
      "own 'interruption in progress' countdown runs; watch mains voltage and charger_enabled fall. You may need to "
      "unplug the cable when the bike prompts you.";
  result.succeeded = true;
  co_return ServiceWriteAnswer{true, result, {}};
}

}  // namespace emcool::vcu
