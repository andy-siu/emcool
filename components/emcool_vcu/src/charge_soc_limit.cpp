#include <cstdio>

#include "emcool/log.hpp"
#include "emcool/text/js.hpp"
#include "emcool/vcu/charge_command.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/write_runner.hpp"

// The bike's "stop charging at N %" setting over the 0x120 dash-command channel — the one write
// action with a REAL read-back, because the channel answers a read with the VCU's stored value
// rather than an echo.

namespace emcool::vcu {

namespace {

constexpr const char* kTag = "vcu-write";
constexpr std::int64_t kWriteSettleMs = 40;
constexpr std::int64_t kReadTimeoutMs = 500;
constexpr std::int64_t kReadPollMs = 10;
constexpr const char* kSocLimitSignal = "charge_soc_limit_pct";

std::string describe(std::optional<int> percent) {
  if (!percent) return "unknown";
  return *percent == 0 ? "no limit" : std::to_string(*percent) + " %";
}

}  // namespace

// Reads the VCU's stored limit, or nullopt if no reply comes. Lives as a free helper so both
// perform methods share it. `read` samples the decoded signal; `send` puts the frame on the bus.
static async::Task<std::optional<int>> readSocLimit(const VcuSend& send, const GateReader& read) {
  const std::array<std::uint8_t, 8> frame = buildChargeSocLimitRead();
  const double markedAt = static_cast<double>(async::currentLoop().now());
  if (const std::optional<std::string> problem = send(kChargeRequestCanId, frame)) {
    logError(kTag, "could not transmit the SOC charge-limit read request: %s", problem->c_str());
    co_return std::nullopt;
  }
  const auto since = [&] { return static_cast<double>(async::currentLoop().now()) - markedAt; };
  while (since() < kReadTimeoutMs) {
    co_await async::sleep(kReadPollMs);
    const GateSample sample = read(kSocLimitSignal);
    if (sample.ageMs && *sample.ageMs <= since()) {
      co_return sample.value ? std::optional<int>(static_cast<int>(*sample.value)) : std::nullopt;
    }
  }
  logWarning(kTag, "no charge-limit reply within %lld ms", static_cast<long long>(kReadTimeoutMs));
  co_return std::nullopt;
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performChargeSocLimit(int percent) {
  const std::optional<int> before = co_await readSocLimit(context_.send, context_.readSignal);
  logWarning(kTag, "about to set the SOC charge limit to %d %% (it reads %s)", percent, describe(before).c_str());

  // sendSocLimit: fire-and-forget; the read that follows is the verification.
  const std::optional<std::array<std::uint8_t, 8>> builtFrame = buildChargeSocLimitWrite(percent);
  std::string sentHex;
  std::optional<std::string> sendFailure;
  if (!builtFrame) {
    sendFailure = "charge-soc-command: " + std::to_string(percent) + " % is outside the 0…" +
                  std::to_string(kMaxSocLimitPct) + " this command carries";
  } else if (const std::optional<std::string> problem = context_.send(kChargeRequestCanId, *builtFrame)) {
    sendFailure = *problem;
  } else {
    char id[12];
    std::snprintf(id, sizeof id, "0x%x", static_cast<unsigned>(kChargeRequestCanId));
    sentHex = std::string(id) + " " + toHex(*builtFrame);
    logWarning(kTag, "sent the SOC charge limit %d %% — %s", percent, sentHex.c_str());
  }

  const auto recordAttempt = [&](const std::string& status, std::optional<int> after, std::optional<std::string> rawHex,
                                 const std::string& note) {
    AuditRecord record;
    record.at = context_.wallClockMs();
    record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
    record.action = AuditAction::kChargeSocLimit;
    record.status = status;
    record.requested = AuditValue::of(static_cast<double>(percent));
    record.before = before ? AuditValue::of(static_cast<double>(*before)) : AuditValue::null();
    record.after = after ? AuditValue::of(static_cast<double>(*after)) : AuditValue::null();
    if (rawHex) record.rawHex = *rawHex;
    record.note = note;
    audit(std::move(record));
  };

  if (sendFailure) {
    recordAttempt("failed", std::nullopt, std::nullopt, *sendFailure);
    co_return ServiceWriteAnswer{false, {}, *sendFailure};
  }

  co_await async::sleep(kWriteSettleMs);
  const std::optional<int> after = co_await readSocLimit(context_.send, context_.readSignal);
  const std::string outcome =
      !after ? "unverified" : (*after == percent ? "written" : "read-back-mismatch");

  std::string note;
  std::string message;
  if (outcome == "written") {
    note = std::to_string(percent) + " % on 0x120 (" + sentHex + "); read back from the VCU's store as " +
           std::to_string(*after) + " %";
    message = "The bike will now stop charging at " + std::to_string(percent) + " % (was " + describe(before) +
              "). Read back from the VCU's own store, not an echo. ⚠️ That it STORES the limit is not proof it stops "
              "there — nothing here has watched the limit be reached — so check the dash and the next full charge.";
  } else if (outcome == "read-back-mismatch") {
    note = "asked for " + std::to_string(percent) + " %, the VCU's store reads " + describe(after) + " afterwards";
    message = "Asked for " + std::to_string(percent) + " %, but the VCU's store reads " + describe(after) +
              ". Nothing was changed as asked; the bike may have clamped or ignored the value. Do not retry blind — "
              "read it on the bike's own screen first.";
  } else {
    note = std::to_string(percent) + " % on 0x120 (" + sentHex +
           "); the write went out and the read-back got NO answer within " + std::to_string(kReadTimeoutMs) +
           " ms — unknown, not failed";
    message = "The " + std::to_string(percent) +
              " % command went out, but the read-back got no answer. ⚠️ This does NOT mean it failed — the write and "
              "the read are separate frames and only the read went unanswered. Read it again, or check the bike's own "
              "screen.";
  }
  recordAttempt(outcome, after, sentHex, note);

  ServiceWriteResult result;
  result.action = AuditAction::kChargeSocLimit;
  result.status = outcome;
  result.message = message;
  result.succeeded = outcome == "written";
  co_return ServiceWriteAnswer{true, result, {}};
}

async::Task<ServiceWriteAnswer> VcuWriteRunner::performChargeSocLimitRead() {
  const std::optional<int> value = co_await readSocLimit(context_.send, context_.readSignal);
  AuditRecord record;
  record.at = context_.wallClockMs();
  record.clockTrustworthy = checkPiClock(context_.clockEvidence()).trustworthy;
  record.action = AuditAction::kChargeSocLimitRead;
  record.status = value ? "read" : "failed";
  record.after = value ? AuditValue::of(static_cast<double>(*value)) : AuditValue::null();
  record.note = value ? "the VCU's store reads " + std::to_string(*value) + " %"
                      : "no reply within " + std::to_string(kReadTimeoutMs) + " ms";
  audit(std::move(record));
  if (!value) {
    co_return ServiceWriteAnswer{
        false, {},
        "the VCU did not answer the charge-limit read within " + std::to_string(kReadTimeoutMs) +
            " ms. The bike may have gone to sleep."};
  }
  ServiceWriteResult result;
  result.action = AuditAction::kChargeSocLimitRead;
  result.status = "read";
  result.message = *value == 0 ? "No charge limit is set — the bike will charge to full."
                               : "The bike is set to stop charging at " + std::to_string(*value) + " %.";
  result.succeeded = true;
  co_return ServiceWriteAnswer{true, result, {}};
}

}  // namespace emcool::vcu
