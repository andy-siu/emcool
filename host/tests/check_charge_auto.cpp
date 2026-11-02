// Replays tools/generate-golden-charge-auto.mts through the C++ port: the automatic-charge control
// law (decideChargeCurrent) and its support (estimateHeatingRate, estimateSocRate,
// sessionAheadMinutes) must produce byte-identical JSON to cool-eva's own code.
//
//   check_charge_auto <golden file>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <map>

#include "check.hpp"
#include "emcool/async/loop.hpp"
#include "emcool/async/task.hpp"
#include "emcool/obd/bus.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/charge_auto.hpp"
#include "emcool/vcu/charge_auto_curve.hpp"

using namespace emcool;
using namespace emcool::vcu;

namespace {

int failures = 0;

std::string build(const std::function<void(text::TextBuffer&)>& write) {
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  write(measure);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

void expectEqual(const std::string& what, const std::string& got, const std::string& want) {
  if (got == want) return;
  if (failures < 20) std::fprintf(stderr, "check_charge_auto: %s mismatch\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(), want.c_str());
  failures++;
}

// A tiny JSON scanner over the compact JSON.stringify output the golden carries. Enough for
// numbers, null, booleans, and arrays of {atMs,celsius}/{atMs,percent}.
struct Scanner {
  const std::string& text;
  std::size_t at = 0;
  explicit Scanner(const std::string& t) : text(t) {}
  void skipWs() { while (at < text.size() && (text[at] == ' ' || text[at] == ',')) at++; }
  bool consume(char c) {
    skipWs();
    if (at < text.size() && text[at] == c) {
      at++;
      return true;
    }
    return false;
  }
  // Read a bare token (number/true/false/null) up to , } ] .
  std::string token() {
    skipWs();
    const std::size_t start = at;
    while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']') at++;
    return text.substr(start, at - start);
  }
  // Find "key": and position at its value. Returns false if not present at the current object level.
  bool field(const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t found = text.find(needle, at);
    if (found == std::string::npos) return false;
    at = found + needle.size();
    return true;
  }
};

std::optional<double> numberField(const std::string& object, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t found = object.find(needle);
  if (found == std::string::npos) return std::nullopt;
  const std::size_t start = found + needle.size();
  if (object.compare(start, 4, "null") == 0) return std::nullopt;
  return std::strtod(object.c_str() + start, nullptr);
}

bool boolField(const std::string& object, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t found = object.find(needle);
  return found != std::string::npos && object.compare(found + needle.size(), 4, "true") == 0;
}

// Parse the array that follows "<key>":[ … ] — elements {"atMs":n,"<valueKey>":n}.
template <typename Sample, typename Make>
std::vector<Sample> sampleArray(const std::string& object, const std::string& key, const std::string& valueKey, Make make) {
  std::vector<Sample> out;
  const std::string needle = "\"" + key + "\":[";
  const std::size_t arr = object.find(needle);
  if (arr == std::string::npos) return out;
  std::size_t cursor = arr + needle.size();
  while (cursor < object.size() && object[cursor] == '{') {
    const std::size_t end = object.find('}', cursor);
    const std::string element = object.substr(cursor, end - cursor + 1);
    out.push_back(make(static_cast<std::int64_t>(*numberField(element, "atMs")), *numberField(element, valueKey)));
    cursor = object.find('{', end);
    const std::size_t close = object.find(']', end);
    if (cursor == std::string::npos || (close != std::string::npos && close < cursor)) break;
  }
  return out;
}

std::vector<TemperatureSample> temperatureSamples(const std::string& object, const std::string& key) {
  return sampleArray<TemperatureSample>(object, key, "celsius",
                                        [](std::int64_t atMs, double v) { return TemperatureSample{atMs, v}; });
}
std::vector<SocSample> socSamples(const std::string& object, const std::string& key) {
  return sampleArray<SocSample>(object, key, "percent", [](std::int64_t atMs, double v) { return SocSample{atMs, v}; });
}

const char* rateKindName(HeatingRate::Kind kind) {
  switch (kind) {
    case HeatingRate::Kind::kRate: return "rate";
    case HeatingRate::Kind::kBounded: return "bounded";
    case HeatingRate::Kind::kNotRising: return "not-rising";
    case HeatingRate::Kind::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string rateJson(const HeatingRate& rate) {
  return build([&](text::TextBuffer& out) {
    out.append("{\"kind\":");
    out.appendJsonString(rateKindName(rate.kind));
    if (rate.kind == HeatingRate::Kind::kRate || rate.kind == HeatingRate::Kind::kBounded) {
      out.append(",\"perMinute\":");
      out.appendNumber(rate.perMinute);
    }
    out.append("}");
  });
}

std::string decisionJson(const ChargeAutoDecision& decision) {
  return build([&](text::TextBuffer& out) {
    if (decision.kind == ChargeAutoDecision::Kind::kCommand) {
      out.append("{\"kind\":\"command\",\"amps\":");
      out.appendInteger(decision.amps);
      out.append(",\"reason\":");
      out.appendInteger(static_cast<int>(decision.reason));
      out.append("}");
    } else {
      out.append("{\"kind\":\"hold\",\"reason\":");
      out.appendInteger(static_cast<int>(decision.reason));
      out.append("}");
    }
  });
}

std::string optNumberJson(std::optional<double> value) {
  if (!value) return "null";
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  measure.appendNumber(*value);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  out.appendNumber(*value);
  text.resize(out.length());
  return text;
}

// A no-op recorder: the loop section asserts commands and state(), not the emitted signal stream.
class NullRecorder final : public obd::SignalRecorder {
 public:
  void record(std::string_view, double) override {}
};

std::string firstField(const std::string& line) { return line.substr(0, line.find(' ')); }

// Split "KIND <a> <rest-with-spaces>" at the first N-1 spaces.
std::vector<std::string> head(const std::string& line, std::size_t count) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (out.size() + 1 < count) {
    const std::size_t space = line.find(' ', at);
    out.push_back(line.substr(at, space - at));
    at = space + 1;
  }
  out.push_back(line.substr(at));
  return out;
}

// --- the stateful controller (loop golden) ----------------------------------------------------

// A fake signal store: value + the monotonic ms it was last recorded, so ageMs = now - lastRecord.
struct FakeSignals {
  std::map<std::string, std::pair<double, std::int64_t>> values;
  std::int64_t now = 0;
  void record(const std::string& key, double value) { values[key] = {value, now}; }
  std::optional<double> latest(std::string_view key) const {
    const auto found = values.find(std::string(key));
    return found == values.end() ? std::nullopt : std::optional<double>(found->second.first);
  }
  std::optional<double> age(std::string_view key) const {
    const auto found = values.find(std::string(key));
    return found == values.end() ? std::nullopt : std::optional<double>(static_cast<double>(now - found->second.second));
  }
};

void runLoopGolden(std::ifstream& input) {
  std::string line;
  // Per-scenario accumulated outputs from the C++ controller, in order.
  struct Expected { std::vector<std::string> lines; };  // the golden's command/state lines, in order

  // Collect lines per scenario, run each.
  std::vector<std::string> block;
  const auto runBlock = [&](std::vector<std::string>& scen) {
    if (scen.empty()) return;
    // A fresh loop per scenario, so the clock resets and no stale interval carries over.
    obd::DeadlineOrder order;
    async::Loop loop(order);
    async::setCurrentLoop(&loop);
    // Parse header.
    const std::vector<std::string> hdr = head(scen.front(), 3);
    const bool enabled = hdr[2] == "1";
    auto signals = std::make_shared<FakeSignals>();
    auto got = std::make_shared<std::vector<std::string>>();   // C++ command/state lines
    std::vector<std::string> want;                              // golden command/state lines

    ChargeAutoReaders readers;
    readers.latestValue = [signals](std::string_view k) { return signals->latest(k); };
    readers.ageMs = [signals](std::string_view k) { return signals->age(k); };
    readers.nowMs = [signals] { return signals->now; };
    NullRecorder recorder;
    ChargeCommandSink sink = [got, signals](int amps) -> async::Task<ChargeCommandResult> {
      got->push_back("command " + std::to_string(signals->now) + " " + std::to_string(amps));
      co_return ChargeCommandResult{true, ""};
    };
    auto controller = std::make_shared<ChargeAutomatic>(enabled, std::move(sink), std::move(readers), recorder);

    const auto emitState = [&] {
      const ChargeAutoState s = controller->state();
      got->push_back("state " + std::to_string(signals->now) + " " + (s.mode == ChargeAutoMode::kAutomatic ? "1" : "0") +
                     " " + std::to_string(static_cast<int>(s.reason)) + " " +
                     (s.commandedAmps ? std::to_string(*s.commandedAmps) : "null"));
    };

    // start() under loop.run so its setInterval and initial publish happen on the loop.
    loop.run(0, [&] { controller->start(); });
    emitState();

    // Replay the input lines in order, advancing the clock; collect golden want-lines.
    for (std::size_t at = 1; at < scen.size(); at++) {
      const std::string& l = scen[at];
      const std::string kind = firstField(l);
      if (kind == "state") {
        want.push_back(l);
        continue;
      }
      const std::vector<std::string> f = head(l, 4);
      const std::int64_t ms = std::strtoll(f[1].c_str(), nullptr, 10);
      if (kind == "command") {
        // A command the golden recorded (possibly from a tick with no input line after it).
        // Advance to its time, firing the interval tick that issues it; the sink appends to `got`.
        want.push_back(l);
        while (loop.nextTimer() && loop.nextTimer()->atMs <= ms) {
          const std::int64_t fireAt = loop.nextTimer()->atMs;
          signals->now = fireAt;
          loop.advanceTo(fireAt);
        }
        signals->now = ms;
        loop.advanceTo(ms);
        continue;
      }
      // Advance the loop to `ms`, firing any interval ticks on the way.
      while (loop.nextTimer() && loop.nextTimer()->atMs <= ms) {
        const std::int64_t fireAt = loop.nextTimer()->atMs;
        signals->now = fireAt;
        loop.advanceTo(fireAt);
      }
      signals->now = ms;
      loop.advanceTo(ms);
      if (kind == "rec") {
        const std::string key = f[2];
        const double value = std::strtod(f[3].c_str(), nullptr);
        signals->record(key, value);
        loop.run(ms, [&] {
          if (key == "batt_temp_hi") {
            controller->observeTemperature(value);
          } else if (key == "soc") {
            controller->observeSoc(value);
          } else if (key == "dc_charge_limit_selected_a") {
            controller->observeSelectedCurrent(value);
          } else if (key == "charge_manager_state") {
            controller->observeSessionState(value);
          }
        });
      } else if (kind == "mode") {
        loop.run(ms, [&] { controller->setMode(f[2] == "automatic" ? ChargeAutoMode::kAutomatic : ChargeAutoMode::kOff); });
      } else if (kind == "note") {
        const std::vector<std::string> nf = head(l, 4);
        loop.run(ms, [&] { controller->noteChargeCurrentOutgoing(std::atoi(nf[2].c_str()), nf[3] == "1"); });
      }
      emitState();
    }
    controller->stop();

    // Compare got vs want, line for line.
    bool ok = got->size() == want.size();
    for (std::size_t at = 0; ok && at < got->size(); at++) ok = (*got)[at] == want[at];
    if (!ok) {
      if (failures < 10) {
        std::fprintf(stderr, "check_charge_auto: loop scenario mismatch (%zu got, %zu want)\n", got->size(), want.size());
        for (std::size_t at = 0; at < got->size() || at < want.size(); at++) {
          std::fprintf(stderr, "    [%zu] got %s  want %s\n", at, at < got->size() ? (*got)[at].c_str() : "(none)",
                       at < want.size() ? want[at].c_str() : "(none)");
        }
      }
      failures++;
    }
  };

  while (std::getline(input, line)) {
    if (line.empty()) continue;
    if (firstField(line) == "scenario") {
      runBlock(block);
      block.clear();
    }
    if (firstField(line) == "end") {
      runBlock(block);
      block.clear();
      continue;
    }
    block.push_back(line);
  }
  runBlock(block);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: check_charge_auto <pure golden> [<loop golden>]\n");
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::fprintf(stderr, "check_charge_auto: cannot open %s\n", argv[1]);
    return 2;
  }
  std::string line;
  int ran = 0;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const std::string kind = firstField(line);
    if (kind == "RATE") {
      // RATE <nowMs> <samples-json> <rate-json>
      const std::vector<std::string> f = head(line, 4);
      const std::int64_t nowMs = std::strtoll(f[1].c_str(), nullptr, 10);
      const std::vector<TemperatureSample> s = temperatureSamples("\"s\":" + f[2], "s");
      expectEqual("RATE", rateJson(estimateHeatingRate(s, nowMs)), f[3]);
    } else if (kind == "SOCRATE") {
      const std::vector<std::string> f = head(line, 4);
      const std::int64_t nowMs = std::strtoll(f[1].c_str(), nullptr, 10);
      const std::vector<SocSample> s = socSamples("\"s\":" + f[2], "s");
      expectEqual("SOCRATE", optNumberJson(estimateSocRate(s, nowMs)), f[3]);
    } else if (kind == "AHEAD") {
      const std::vector<std::string> f = head(line, 3);
      const std::string& obj = f[1];
      const std::optional<double> ahead =
          sessionAheadMinutes(numberField(obj, "socPercent"), numberField(obj, "socAgeMs"),
                              socSamples(obj, "socSamples"), numberField(obj, "requestedAmps"),
                              static_cast<std::int64_t>(*numberField(obj, "nowMs")));
      expectEqual("AHEAD", optNumberJson(ahead), f[2]);
    } else if (kind == "DECIDE") {
      const std::vector<std::string> f = head(line, 3);
      const std::string& obj = f[1];
      ChargeAutoInput in;
      in.enabled = boolField(obj, "enabled");
      in.packTemperatureC = numberField(obj, "packTemperatureC");
      in.packTemperatureAgeMs = numberField(obj, "packTemperatureAgeMs");
      in.packTemperaturePlausible = boolField(obj, "packTemperaturePlausible");
      in.chargeManagerState = numberField(obj, "chargeManagerState");
      in.chargeManagerStateAgeMs = numberField(obj, "chargeManagerStateAgeMs");
      in.ceilingAmps = numberField(obj, "ceilingAmps");
      in.commandedAmps = numberField(obj, "commandedAmps");
      in.riderOverride = boolField(obj, "riderOverride");
      in.samples = temperatureSamples(obj, "samples");
      in.socPercent = numberField(obj, "socPercent");
      in.socAgeMs = numberField(obj, "socAgeMs");
      in.socSamples = socSamples(obj, "socSamples");
      in.requestedAmps = numberField(obj, "requestedAmps");
      if (const std::optional<double> last = numberField(obj, "lastCommandAtMs")) in.lastCommandAtMs = static_cast<std::int64_t>(*last);
      in.nowMs = static_cast<std::int64_t>(*numberField(obj, "nowMs"));
      expectEqual("DECIDE", decisionJson(decideChargeCurrent(in)), f[2]);
    }
    ran++;
  }

  int loopScenarios = 0;
  if (argc >= 3) {
    std::ifstream loopInput(argv[2]);
    if (!loopInput) {
      std::fprintf(stderr, "check_charge_auto: cannot open %s\n", argv[2]);
      return 2;
    }
    runLoopGolden(loopInput);
    loopInput.clear();
    loopInput.seekg(0);
    std::string l;
    while (std::getline(loopInput, l)) if (l.rfind("scenario", 0) == 0) loopScenarios++;
  }

  if (failures > 0) {
    std::fprintf(stderr, "check_charge_auto: %d failures\n", failures);
    return 1;
  }
  std::fprintf(stderr,
               "check_charge_auto: %d pure lines + %d controller scenarios match cool-eva; 0 failures\n", ran,
               loopScenarios);
  return 0;
}
