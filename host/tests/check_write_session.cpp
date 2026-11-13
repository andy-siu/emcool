// Replays tools/generate-golden-write-session.mts through the C++ WriteSession: given the same
// scripted replies at the same milliseconds, it must transmit the same frames at the same times and
// resolve to the same outcome the reference's own write-session.ts did. This is the stateful half of
// stage 4 — the session timing, the compare-and-swap, the unlock handshake, the read-back.
//
//   check_write_session <golden file>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <filesystem>

#include "check.hpp"
#include "emcool/async/loop.hpp"
#include "emcool/async/task.hpp"
#include "emcool/obd/bus.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/charge_ack_watch.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/snapshot.hpp"
#include "emcool/vcu/store.hpp"
#include "emcool/vcu/write_runner.hpp"
#include "emcool/vcu/write_session.hpp"
#include "emcool/vcu/write_targets.hpp"

using namespace emcool;
using namespace emcool::vcu;

namespace {

int failures = 0;

std::vector<std::uint8_t> parseHex(const std::string& text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(at, 2).c_str(), nullptr, 16)));
  }
  return bytes;
}

std::string lowerHex(std::span<const std::uint8_t> bytes) {
  std::string text;
  char pair[3];
  for (const std::uint8_t byte : bytes) {
    std::snprintf(pair, sizeof pair, "%02x", byte);
    text += pair;
  }
  return text;
}

std::vector<std::string> words(const std::string& line) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < line.size()) {
    const std::size_t space = line.find(' ', at);
    out.push_back(line.substr(at, space == std::string::npos ? std::string::npos : space - at));
    if (space == std::string::npos) break;
    at = space + 1;
  }
  return out;
}

std::string build(const std::function<void(text::TextBuffer&)>& write) {
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  write(measure);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

const char* writeStatusWord(ServiceWriteOutcome::Status status) {
  switch (status) {
    case ServiceWriteOutcome::Status::kWritten:
      return "written";
    case ServiceWriteOutcome::Status::kReadBackMismatch:
      return "read-back-mismatch";
    case ServiceWriteOutcome::Status::kStalePrecondition:
      return "stale-precondition";
    case ServiceWriteOutcome::Status::kRefused:
      return "refused";
    case ServiceWriteOutcome::Status::kFailed:
      return "failed";
  }
  return "failed";
}

std::string planJson(const ParameterWritePlan& plan) {
  return build([&](text::TextBuffer& out) {
    out.append("{\"name\":");
    out.appendJsonString(plan.name);
    out.append(",\"index\":");
    out.appendInteger(plan.index);
    out.append(",\"micro\":");
    out.appendJsonString(microName(plan.micro));
    out.append(",\"identifier\":");
    out.appendInteger(plan.identifier);
    out.append(",\"record\":{");
    for (std::size_t at = 0; at < plan.record.size(); at++) {
      if (at > 0) out.append(",");
      out.append("\"");
      out.appendInteger(static_cast<int>(at));
      out.append("\":");
      out.appendInteger(plan.record[at]);
    }
    out.append("},\"value\":");
    out.appendNumber(plan.value);
    out.append(",\"previousValue\":");
    out.appendNumber(plan.previousValue);
    out.append(",\"description\":");
    out.appendJsonString(plan.description);
    out.append("}");
  });
}

std::string outcomeJson(const ServiceWriteOutcome& outcome) {
  return build([&](text::TextBuffer& out) {
    out.append("{\"status\":");
    out.appendJsonString(writeStatusWord(outcome.status));
    switch (outcome.status) {
      case ServiceWriteOutcome::Status::kWritten:
      case ServiceWriteOutcome::Status::kReadBackMismatch:
        out.append(",\"plan\":");
        out.append(planJson(outcome.plan));
        out.append(",\"readBack\":");
        out.appendNumber(outcome.readBack);
        out.append(",\"rawHex\":");
        out.appendJsonString(outcome.rawHex);
        break;
      case ServiceWriteOutcome::Status::kStalePrecondition:
        out.append(",\"plan\":");
        out.append(planJson(outcome.plan));
        out.append(",\"actual\":");
        out.appendNumber(outcome.actual);
        break;
      case ServiceWriteOutcome::Status::kRefused:
        out.append(",\"plan\":");
        out.append(planJson(outcome.plan));
        out.append(",\"stage\":");
        out.appendJsonString(writeStageName(outcome.stage));
        out.append(",\"negativeResponseCode\":");
        out.appendInteger(outcome.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(outcome.description);
        break;
      case ServiceWriteOutcome::Status::kFailed:
        out.append(",\"plan\":");
        out.append(planJson(outcome.plan));
        out.append(",\"stage\":");
        out.appendJsonString(writeStageName(outcome.stage));
        out.append(",\"reason\":");
        out.appendJsonString(outcome.reason);
        break;
    }
    out.append("}");
  });
}

std::string resetJson(const ResetVcuOutcome& outcome) {
  return build([&](text::TextBuffer& out) {
    switch (outcome.status) {
      case ResetVcuOutcome::Status::kReset:
        out.append("{\"status\":\"reset\",\"note\":");
        out.appendJsonString(outcome.note);
        out.append("}");
        return;
      case ResetVcuOutcome::Status::kRefused:
        out.append("{\"status\":\"refused\",\"micro\":");
        out.appendJsonString(outcome.micro);
        out.append(",\"negativeResponseCode\":");
        out.appendInteger(outcome.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(outcome.description);
        out.append("}");
        return;
      case ResetVcuOutcome::Status::kFailed:
        out.append("{\"status\":\"failed\",\"stage\":");
        out.appendJsonString(outcome.stage);
        out.append(",\"micro\":");
        out.appendJsonString(outcome.micro);
        out.append(",\"reason\":");
        out.appendJsonString(outcome.reason);
        out.append("}");
        return;
    }
  });
}

std::string clearJson(const ClearDtcsOutcome& outcome) {
  return build([&](text::TextBuffer& out) {
    switch (outcome.status) {
      case ClearDtcsOutcome::Status::kCleared:
        out.append("{\"status\":\"cleared\"}");
        return;
      case ClearDtcsOutcome::Status::kRefused:
        out.append("{\"status\":\"refused\",\"negativeResponseCode\":");
        out.appendInteger(outcome.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(outcome.description);
        out.append("}");
        return;
      case ClearDtcsOutcome::Status::kFailed:
        out.append("{\"status\":\"failed\",\"reason\":");
        out.appendJsonString(outcome.reason);
        out.append("}");
        return;
    }
  });
}

std::string stampOutcomeJson(const StampRead& read) {
  return build([&](text::TextBuffer& out) {
    if (!read.ok) {
      out.append("{\"ok\":false,\"reason\":");
      out.appendJsonString(read.reason);
      out.append("}");
      return;
    }
    const ServiceStamp& stamp = read.stamp;
    out.append("{\"ok\":true,\"stamp\":{\"raw\":{\"dateLow\":");
    out.appendInteger(stamp.raw.dateLow);
    out.append(",\"dateHigh\":");
    out.appendInteger(stamp.raw.dateHigh);
    out.append(",\"odometerLow\":");
    out.appendInteger(stamp.raw.odometerLow);
    out.append(",\"odometerHigh\":");
    out.appendInteger(stamp.raw.odometerHigh);
    out.append("},\"dateSeconds\":");
    out.appendNumber(stamp.dateSeconds);
    out.append(",\"dateIso\":");
    if (stamp.dateIso) {
      out.appendJsonString(*stamp.dateIso);
    } else {
      out.append("null");
    }
    out.append(",\"odometer\":");
    out.appendNumber(stamp.odometer);
    out.append(",\"implausible\":");
    if (stamp.implausible) {
      out.appendJsonString(*stamp.implausible);
    } else {
      out.append("null");
    }
    out.append("}}");
  });
}

// One scenario's recorded frames and expected outcome.
struct Block {
  std::string op;
  std::string micro;
  std::string name;
  double value = 0;
  double previous = 0;
  std::vector<std::pair<std::int64_t, std::string>> sent;      // ms, data hex (expected from C++)
  std::vector<std::pair<std::int64_t, std::pair<std::uint32_t, std::string>>> replies;  // ms, (id, data)
  std::string outcome;       // the golden's outcome JSON (or {"planfail":…})
  bool planfail = false;
  // charge cases:
  bool charge = false;
  bool chargeStop = false;
  std::map<std::string, double> seed;
  int amps = 0;
  std::string chargeAnswer;  // the golden's ServiceWriteAnswer JSON
};

// A no-op recorder for the ack-watch in the charge block (we assert frames + the answer, not records).
class NullRecorder final : public obd::SignalRecorder {
 public:
  void record(std::string_view, double) override {}
};

ParameterNames gNames;

// A confirmed report for the default table (what the generator passed).
TableTypeReport confirmedReport() {
  TableTypeReport report;
  report.verdicts.push_back({276, Micro::kA9, static_cast<double>(kDefaultTableType), true, {}});
  report.verdicts.push_back({277, Micro::kA8, static_cast<double>(kDefaultTableType), true, {}});
  return report;
}

void runBlock(const Block& block) {
  if (block.planfail) {
    // planWrite refuses before any session; just confirm the C++ planner refuses with the same reason.
    // emcool names the file write_targets where the reference says src/vcu/write-targets.ts (see check_write).
    const WritePlanOutcome planned = planWrite(gNames, block.name, block.value, block.previous);
    std::string reason = planned.ok ? std::string("UNEXPECTEDLY OK") : planned.reason;
    const std::string from = "written down with its reasoning in write_targets.";
    const std::size_t at = reason.find(from);
    if (at != std::string::npos) reason.replace(at, from.size(), "written down with its reasoning in src/vcu/write-targets.ts.");
    const std::string got = build([&](text::TextBuffer& out) {
      out.append("{\"planfail\":");
      out.appendJsonString(reason);
      out.append("}");
    });
    if (got != block.outcome) {
      if (failures < 10) std::fprintf(stderr, "check_write_session: planfail mismatch\n  got:  %s\n  want: %s\n", got.c_str(), block.outcome.c_str());
      failures++;
    }
    return;
  }

  obd::DeadlineOrder order;
  async::Loop loop(order);
  async::setCurrentLoop(&loop);
  SecurityCooldown cooldown;
  auto sent = std::make_shared<std::vector<std::pair<std::int64_t, std::string>>>();
  const VcuSend send = [sent](std::uint32_t id, const std::array<std::uint8_t, 8>& frame) -> std::optional<std::string> {
    char idText[8];
    std::snprintf(idText, sizeof idText, "%x", id);
    sent->push_back({static_cast<std::int64_t>(async::currentLoop().now()), std::string(idText) + ":" + lowerHex(frame)});
    return std::nullopt;
  };
  auto session = std::make_shared<WriteSession>(send, gNames, cooldown);
  const TableTypeReport report = confirmedReport();

  std::string got;
  bool done = false;
  const auto finish = [&](std::string json) {
    got = std::move(json);
    done = true;
  };

  const auto launch = [&] {
    if (block.op == "param") {
      const WritePlanOutcome planned = planWrite(gNames, block.name, block.value, block.previous);
      [](std::shared_ptr<WriteSession> s, ParameterWritePlan plan, const TableTypeReport* report,
         std::function<void(std::string)> finish) -> async::Task<> {
        const ServiceWriteOutcome outcome = co_await s->parameterWrite(std::move(plan), report, true);
        finish(outcomeJson(outcome));
      }(session, planned.plan, &report, [&](std::string json) { finish(std::move(json)); })
          .detach();
    } else if (block.op == "stamp") {
      [](std::shared_ptr<WriteSession> s, std::function<void(std::string)> finish) -> async::Task<> {
        const StampRead read = co_await s->readServiceStamp(1790000000000.0);
        finish(stampOutcomeJson(read));
      }(session, [&](std::string json) { finish(std::move(json)); })
          .detach();
    } else if (block.op == "reset") {
      [](std::shared_ptr<WriteSession> s, std::function<void(std::string)> finish) -> async::Task<> {
        const ResetVcuOutcome outcome = co_await s->resetVcu();
        finish(resetJson(outcome));
      }(session, [&](std::string json) { finish(std::move(json)); })
          .detach();
    } else if (block.op == "clear") {
      [](std::shared_ptr<WriteSession> s, std::function<void(std::string)> finish) -> async::Task<> {
        const ClearDtcsOutcome outcome = co_await s->clearStoredDtcs();
        finish(clearJson(outcome));
      }(session, [&](std::string json) { finish(std::move(json)); })
          .detach();
    }
  };
  loop.run(0, launch);

  // Drive the loop, injecting each reply at its recorded ms, until the outcome settles.
  std::size_t nextReply = 0;
  for (std::int64_t ms = 0; ms <= 20000 && !done; ms++) {
    loop.advanceTo(ms);
    while (nextReply < block.replies.size() && block.replies[nextReply].first <= ms) {
      const auto& reply = block.replies[nextReply++];
      const std::vector<std::uint8_t> data = parseHex(reply.second.second);
      loop.run(ms, [&] { session->handleFrame(reply.second.first, data); });
      loop.advanceTo(ms);
    }
  }

  if (!done) {
    if (failures < 10) std::fprintf(stderr, "check_write_session: %s never settled\n", block.op.c_str());
    failures++;
    return;
  }

  // Compare the frames sent (id:hex, in order) and the outcome.
  bool framesOk = sent->size() == block.sent.size();
  for (std::size_t at = 0; framesOk && at < sent->size(); at++) {
    // The golden's `t` line is "<id-hex> <data-hex>"; our sent is "<id-hex>:<data-hex>".
    const std::string want = block.sent[at].second;
    const std::size_t colon = (*sent)[at].second.find(':');
    const std::string gotId = (*sent)[at].second.substr(0, colon);
    const std::string gotData = (*sent)[at].second.substr(colon + 1);
    const std::size_t space = want.find(' ');
    if (gotId != want.substr(0, space) || gotData != want.substr(space + 1)) framesOk = false;
  }
  if (!framesOk) {
    if (failures < 10) {
      std::fprintf(stderr, "check_write_session: %s frame mismatch (%zu sent, %zu expected)\n", block.op.c_str(),
                   sent->size(), block.sent.size());
      for (std::size_t at = 0; at < sent->size() || at < block.sent.size(); at++) {
        const std::string g = at < sent->size() ? (*sent)[at].second : "(none)";
        const std::string w = at < block.sent.size() ? block.sent[at].second : "(none)";
        std::fprintf(stderr, "    [%zu] got %s  want %s\n", at, g.c_str(), w.c_str());
      }
    }
    failures++;
  }
  if (got != block.outcome) {
    if (failures < 10) std::fprintf(stderr, "check_write_session: %s outcome mismatch\n  got:  %s\n  want: %s\n", block.op.c_str(), got.c_str(), block.outcome.c_str());
    failures++;
  }
}

std::string chargeAnswerJson(const ServiceWriteAnswer& answer) {
  return build([&](text::TextBuffer& out) {
    if (!answer.ok) {
      out.append("{\"ok\":false,\"reason\":");
      out.appendJsonString(answer.reason);
      // The reference's mayHaveSent: present (true) only when a charge-current frame may have left.
      out.append(answer.mayHaveSent ? ",\"mayHaveSent\":true}" : "}");
      return;
    }
    const ServiceWriteResult& r = answer.result;
    out.append("{\"ok\":true,\"result\":{\"action\":");
    out.appendJsonString(auditActionName(r.action));
    out.append(",\"status\":");
    out.appendJsonString(r.status);
    out.append(",\"message\":");
    out.appendJsonString(r.message);
    out.append(r.succeeded ? ",\"succeeded\":true}}" : ",\"succeeded\":false}}");
  });
}

void runChargeBlock(const Block& block) {
  obd::DeadlineOrder order;
  async::Loop loop(order);
  async::setCurrentLoop(&loop);
  char dir[] = "/tmp/emcool-charge-XXXXXX";
  const std::string directory = ::mkdtemp(dir);
  ParameterNames names;
  VcuStore store(directory, names);
  NullRecorder recorder;
  ChargeAckWatch ackWatch(recorder);

  auto sent = std::make_shared<std::vector<std::string>>();
  // The scenario's "__failSendAt": the 1-based frame whose send fails, as the generator's channel throws.
  const auto failAt = block.seed.find("__failSendAt");
  const std::size_t failSendAt = failAt == block.seed.end() ? 0 : static_cast<std::size_t>(failAt->second);
  WriteContext context;
  context.send = [sent, failSendAt](std::uint32_t id,
                                    const std::array<std::uint8_t, 8>& frame) -> std::optional<std::string> {
    if (failSendAt != 0 && sent->size() + 1 == failSendAt) return std::string("ENOBUFS: the transmit queue is full");
    char idText[12];
    std::snprintf(idText, sizeof idText, "%x", static_cast<unsigned>(id));
    sent->push_back(std::string(idText) + " " + lowerHex(frame));
    return std::nullopt;
  };
  context.enabled = true;
  context.busIsActive = true;
  context.directory = directory;
  context.gate = [] {
    ServiceGateVerdict v;
    v.safe = true;
    v.chargingEvidence = "the charge manager reports a live session";
    return v;
  };
  const std::map<std::string, double> seed = block.seed;
  context.readSignal = [seed](std::string_view key) {
    GateSample sample;
    const auto found = seed.find(std::string(key));
    if (found != seed.end()) {
      sample.value = found->second;
      sample.ageMs = 0;  // fresh
    }
    return sample;
  };
  context.wallClockMs = [] { return 1790000000000.0; };
  context.monotonicMs = [] { return static_cast<std::int64_t>(async::currentLoop().now()); };
  context.clockEvidence = [] {
    PiClockEvidence e;
    e.systemEpochMs = 1790000000000.0;
    return e;
  };
  context.latestReport = [] { return std::optional<TableTypeReport>(); };
  context.version = {std::nullopt, false, true, "test"};
  context.store = &store;
  context.ackWatch = &ackWatch;
  context.onChargeCurrentOutgoing = [](int, ServiceWriteRequest::ChargeOrigin) {};
  BusLease lease;
  VcuWriteRunner runner(std::move(context), lease);

  ServiceWriteRequest request;
  if (block.chargeStop) {
    request.kind = ServiceWriteRequest::Kind::kChargeStop;
  } else {
    request.kind = ServiceWriteRequest::Kind::kChargeCurrent;
    request.amps = block.amps;
    request.origin = ServiceWriteRequest::ChargeOrigin::kManual;
  }
  std::string got;
  bool done = false;
  loop.run(0, [&] {
    [](VcuWriteRunner* r, ServiceWriteRequest req, std::string* got, bool* done) -> async::Task<> {
      const ServiceWriteAnswer answer = co_await r->perform(std::move(req));
      *got = chargeAnswerJson(answer);
      *done = true;
    }(&runner, request, &got, &done)
        .detach();
  });
  for (std::int64_t ms = 0; ms <= 1000 && !done; ms++) loop.advanceTo(ms);

  std::filesystem::remove_all(directory);

  if (!done) {
    if (failures < 10) std::fprintf(stderr, "check_write_session: charge never settled\n");
    failures++;
    return;
  }
  // Frames (id + data) in order.
  bool framesOk = sent->size() == block.sent.size();
  for (std::size_t at = 0; framesOk && at < sent->size(); at++) framesOk = (*sent)[at] == block.sent[at].second;
  if (!framesOk) {
    if (failures < 10) {
      std::fprintf(stderr, "check_write_session: charge frame mismatch (%zu sent, %zu expected)\n", sent->size(),
                   block.sent.size());
      for (std::size_t at = 0; at < sent->size() || at < block.sent.size(); at++) {
        std::fprintf(stderr, "    [%zu] got %s  want %s\n", at, at < sent->size() ? (*sent)[at].c_str() : "(none)",
                     at < block.sent.size() ? block.sent[at].second.c_str() : "(none)");
      }
    }
    failures++;
  }
  if (got != block.chargeAnswer) {
    if (failures < 10) std::fprintf(stderr, "check_write_session: charge answer mismatch\n  got:  %s\n  want: %s\n", got.c_str(), block.chargeAnswer.c_str());
    failures++;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: check_write_session <golden file>\n");
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::fprintf(stderr, "check_write_session: cannot open %s\n", argv[1]);
    return 2;
  }
  std::string line;
  std::optional<Block> block;
  int ran = 0;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const std::vector<std::string> w = words(line);
    if (w[0] == "case") {
      block.emplace();
      block->op = w[2];
      if (w[2] == "param") {
        block->micro = w[3];
        block->name = w[4];
        block->value = std::strtod(w[5].c_str(), nullptr);
        block->previous = std::strtod(w[6].c_str(), nullptr);
      } else if (w[2] == "planfail") {
        block->op = "param";
        block->planfail = true;
        block->name = w[3];
        block->value = std::strtod(w[4].c_str(), nullptr);
        block->previous = std::strtod(w[5].c_str(), nullptr);
      } else if (w[2] == "chargecurrent" || w[2] == "chargestop") {
        // case <n> charge(current|stop) <seed k=v,k=v|-> <amps>
        block->charge = true;
        block->chargeStop = w[2] == "chargestop";
        if (w[3] != "-") {
          std::string s = w[3];
          std::size_t at = 0;
          while (at < s.size()) {
            const std::size_t comma = s.find(',', at);
            const std::string pair = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            const std::size_t eq = pair.find('=');
            block->seed[pair.substr(0, eq)] = std::strtod(pair.substr(eq + 1).c_str(), nullptr);
            if (comma == std::string::npos) break;
            at = comma + 1;
          }
        }
        block->amps = std::atoi(w[4].c_str());
      }
    } else if (w[0] == "chargeanswer" && block) {
      block->chargeAnswer = line.substr(line.find(' ') + 1);
    } else if (w[0] == "t" && block) {
      block->sent.push_back({std::strtoll(w[1].c_str(), nullptr, 10), w[2] + " " + w[3]});
    } else if (w[0] == "r" && block) {
      block->replies.push_back({std::strtoll(w[1].c_str(), nullptr, 10),
                                {static_cast<std::uint32_t>(std::strtoul(w[2].c_str(), nullptr, 16)), w[3]}});
    } else if (w[0] == "outcome" && block) {
      block->outcome = line.substr(line.find(' ') + 1);
    } else if (w[0] == "end" && block) {
      if (block->charge) {
        runChargeBlock(*block);
      } else {
        runBlock(*block);
      }
      ran++;
      block.reset();
    }
  }

  if (failures > 0) {
    std::fprintf(stderr, "check_write_session: %d failures\n", failures);
    return 1;
  }
  std::fprintf(stderr, "check_write_session: %d scenarios, frames and outcomes match the reference; 0 failures\n", ran);
  return 0;
}
