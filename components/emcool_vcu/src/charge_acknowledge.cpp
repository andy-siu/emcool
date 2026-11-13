#include "emcool/vcu/charge_acknowledge.hpp"

#include <algorithm>
#include <limits>

#include "emcool/text/js.hpp"

namespace emcool::vcu {

namespace {
constexpr double kAmpTolerance = 1;
}

ChargeAckVerdict judgeChargeCommand(const ChargeAckInput& input) {
  const double commandedAmps = input.commandedAmps;
  const std::int64_t sentAtMs = input.sentAtMs;
  const std::int64_t nowMs = input.nowMs;
  const std::int64_t settleMs = input.settleMs;

  std::vector<AckSample> before;
  for (const AckSample& sample : input.samples) {
    if (sample.atMs <= sentAtMs && sample.atMs >= sentAtMs - kAckSampleHistoryMs) before.push_back(sample);
  }
  ChargeAckVerdict verdict;
  if (before.empty()) {
    verdict.kind = ChargeAckVerdict::Kind::kNoEvidence;
    return verdict;
  }
  const AckSample lastBefore = before.back();
  const bool supersededEarly = input.hasSuperseded && input.supersededAtMs <= sentAtMs + kAckTimeoutMs;
  const double windowEnds =
      std::min(static_cast<double>(sentAtMs + kAckTimeoutMs),
               input.hasSuperseded ? static_cast<double>(input.supersededAtMs) : std::numeric_limits<double>::infinity());
  std::vector<AckSample> after;
  for (const AckSample& sample : input.samples) {
    if (sample.atMs > sentAtMs && static_cast<double>(sample.atMs) <= windowEnds) after.push_back(sample);
  }

  if (lastBefore.amps > commandedAmps + kAmpTolerance) {
    std::vector<AckSample> settled;
    for (const AckSample& sample : after) {
      if (sample.atMs >= sentAtMs + settleMs) settled.push_back(sample);
    }
    double heldAmps = lastBefore.amps;
    if (!settled.empty()) {
      heldAmps = settled.front().amps;
      for (const AckSample& sample : settled) heldAmps = std::max(heldAmps, sample.amps);
    }
    if (!settled.empty() && heldAmps <= commandedAmps + kAmpTolerance) {
      const AckSample* firstAtOrUnder = nullptr;
      for (const AckSample& sample : after) {
        if (sample.amps <= commandedAmps + kAmpTolerance) {
          firstAtOrUnder = &sample;
          break;
        }
      }
      const AckSample& chosen = firstAtOrUnder != nullptr ? *firstAtOrUnder : settled.front();
      verdict.kind = ChargeAckVerdict::Kind::kTook;
      verdict.fromAmps = lastBefore.amps;
      verdict.toAmps = heldAmps;
      verdict.latencyMs = chosen.atMs - sentAtMs;
      return verdict;
    }
    if (nowMs < windowEnds) {
      verdict.kind = ChargeAckVerdict::Kind::kWaiting;
      verdict.elapsedMs = nowMs - sentAtMs;
      return verdict;
    }
    if (supersededEarly) {
      verdict.kind = ChargeAckVerdict::Kind::kSuperseded;
      return verdict;
    }
    verdict.kind = ChargeAckVerdict::Kind::kNotAcknowledged;
    verdict.heldAmps = heldAmps;
    return verdict;
  }

  if (commandedAmps > lastBefore.amps + kAmpTolerance) {
    const AckSample* reached = nullptr;
    for (const AckSample& sample : after) {
      if (sample.amps >= commandedAmps - kAmpTolerance) {
        reached = &sample;
        break;
      }
    }
    bool allUnder = true;
    for (const AckSample& sample : after) {
      if (sample.amps > commandedAmps + kAmpTolerance) allUnder = false;
    }
    if (reached != nullptr && allUnder) {
      verdict.kind = ChargeAckVerdict::Kind::kTook;
      verdict.fromAmps = lastBefore.amps;
      verdict.toAmps = reached->amps;
      verdict.latencyMs = reached->atMs - sentAtMs;
      return verdict;
    }
    if (nowMs < windowEnds) {
      verdict.kind = ChargeAckVerdict::Kind::kWaiting;
      verdict.elapsedMs = nowMs - sentAtMs;
      return verdict;
    }
    if (supersededEarly) {
      verdict.kind = ChargeAckVerdict::Kind::kSuperseded;
      return verdict;
    }
    // The highest reading after, or lastBefore's if there is none — NOT seeded with lastBefore.
    double best = lastBefore.amps;
    if (!after.empty()) {
      best = after.front().amps;
      for (const AckSample& sample : after) best = std::max(best, sample.amps);
    }
    verdict.kind = ChargeAckVerdict::Kind::kStationLimited;
    verdict.askedAmps = commandedAmps;
    verdict.gotAmps = best;
    return verdict;
  }

  verdict.kind = ChargeAckVerdict::Kind::kNoChangeExpected;
  verdict.atAmps = lastBefore.amps;
  return verdict;
}

int chargeAckCode(ChargeAckVerdict::Kind kind) {
  switch (kind) {
    case ChargeAckVerdict::Kind::kWaiting:
      return 0;
    case ChargeAckVerdict::Kind::kTook:
      return 1;
    case ChargeAckVerdict::Kind::kNotAcknowledged:
      return 2;
    case ChargeAckVerdict::Kind::kStationLimited:
      return 3;
    case ChargeAckVerdict::Kind::kNoChangeExpected:
      return 4;
    case ChargeAckVerdict::Kind::kSuperseded:
      return 5;
    case ChargeAckVerdict::Kind::kNoEvidence:
      return 6;
  }
  return 6;
}

std::string describeChargeAck(const ChargeAckVerdict& verdict, double commandedAmps) {
  switch (verdict.kind) {
    case ChargeAckVerdict::Kind::kWaiting:
      return "Sent " + text::jsNumber(commandedAmps) + " A — waiting for the bike to answer (" +
             text::jsNumber(text::jsRound(verdict.elapsedMs / 1000.0)) + " s).";
    case ChargeAckVerdict::Kind::kTook:
      return "Command took: request " + text::jsNumber(verdict.fromAmps) + " → " + text::jsNumber(verdict.toAmps) +
             " A in " + text::jsToFixed(verdict.latencyMs / 1000.0, 1) + " s.";
    case ChargeAckVerdict::Kind::kNotAcknowledged:
      return "Command NOT acknowledged after " + text::jsNumber(kAckTimeoutMs / 1000) + " s — the request is still " +
             text::jsNumber(verdict.heldAmps) + " A.";
    case ChargeAckVerdict::Kind::kStationLimited:
      return "Station-limited: asked " + text::jsNumber(verdict.askedAmps) + " A, the charger is giving " +
             text::jsNumber(verdict.gotAmps) +
             " A. Not a fault — only a reduction proves a command took.";
    case ChargeAckVerdict::Kind::kNoChangeExpected:
      return "Sent " + text::jsNumber(commandedAmps) + " A — nothing to observe, the request was already " +
             text::jsNumber(verdict.atAmps) + " A.";
    case ChargeAckVerdict::Kind::kSuperseded:
      return "A newer command replaced this one before the bike answered.";
    case ChargeAckVerdict::Kind::kNoEvidence:
      return "No reading of the charge request yet, so whether it took cannot be said.";
  }
  return {};
}

}  // namespace emcool::vcu
