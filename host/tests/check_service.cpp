// Replays tools/generate-golden-service.mts through the C++ port: the OBD stack and service mode's
// read runner on one event loop, as on the bike, fed the same frames, gate signals and dashboard
// actions at the same milliseconds. In every millisecond it must send the same frames, record the
// same signals, log the same lines and answer the same JSON as cool-eva did.
//
// The snapshots go to a real directory; cool-eva's went to an in-memory one called /vcu-params, as
// JSON rather than JSON lines, with latest.json where emcool keeps latest.txt. Those three are
// translated in what the C++ says before comparing. Log lines are compared without their tags.
//
//   check_service <golden file>

#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/async/task.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/obd/obd_stack.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/bodies.hpp"
#include "emcool/vcu/runner.hpp"
#include "emcool/vcu/service_api.hpp"

using namespace emcool;
using namespace emcool::vcu;

namespace {

std::vector<std::uint8_t> parseHex(const std::string& text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(at, 2).c_str(), nullptr, 16)));
  }
  return bytes;
}

std::string toHexLower(std::span<const std::uint8_t> bytes) {
  std::string text;
  char pair[3];
  for (const std::uint8_t byte : bytes) {
    std::snprintf(pair, sizeof pair, "%02x", byte);
    text += pair;
  }
  return text.empty() ? "-" : text;
}

std::vector<std::string> fields(const std::string& line, std::size_t count) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (out.size() + 1 < count) {
    const std::size_t space = line.find(' ', at);
    if (space == std::string::npos) break;
    out.push_back(line.substr(at, space - at));
    at = space + 1;
  }
  out.push_back(line.substr(at));
  return out;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
  if (from.empty()) return text;
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

std::string formatValue(double value) {
  char text[40];
  std::snprintf(text, sizeof text, "%.17g", value);
  return text;
}

template <typename Write>
std::string build(Write write) {
  std::string text(64 * 1024, '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

struct Expected {
  std::string text;
  bool fail = false;
  bool consumed = false;
};

struct Millisecond {
  std::vector<std::string> inputs;  // "g <key> <value>" and "a <action…>", in order
  std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> arrivals;
  std::vector<Expected> outputs;
};

// cool-eva's two pointers at its extract script, as emcool words them — plain in logs, escaped in JSON.
std::string emcoolWording(std::string text) {
  for (const char* quote : {"\"", "\\\""}) {
    text = replaceAll(text,
                      std::string("Add the table with scripts/extract-vcu-tables.ts (docs/vcu-parameters.md: ") + quote +
                          "Adding your bike's VCU parameter table" + quote + ").",
                      std::string("Add the table to cool-eva (its docs/vcu-parameters.md: ") + quote +
                          "Adding your bike's VCU parameter table" + quote + ") and run tools/generate-vcu-tables.mts.");
  }
  text = replaceAll(text, "switched off on this Pi (SERVICE_MODE_ENABLED=0)", "switched off on this bike (SERVICE_MODE_ENABLED=0)");
  // cool-eva now points at its dashboard Settings page; on emcool the switch is still OBD_ENABLED.
  text = replaceAll(text, "the CAN channel cannot transmit — check the adapter and bus configuration",
                    "the bus is listen-only (OBD_ENABLED=0) — nothing can be transmitted, so every read would time out");
  text = replaceAll(text, "node --experimental-strip-types scripts/read-freeze-frame.ts --lifetime --save",
                    emcool::vcu::kLifetimeHowToReadWithServiceStopped);
  return replaceAll(text,
                    "run scripts/extract-vcu-tables.ts against your own service-tool install (see docs/vcu-parameters.md) and send "
                    "the diff.",
                    "add it to cool-eva (see its docs/vcu-parameters.md) and run tools/generate-vcu-tables.mts.");
}

// A log line as compared: the message without its "tag: ".
std::string messageOf(const std::string& text) {
  const std::size_t colon = text.find(": ");
  if (colon == std::string::npos || colon == 0) return text;
  for (std::size_t at = 0; at < colon; at++) {
    const char character = text[at];
    if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-')) return text;
  }
  return text.substr(colon + 2);
}

class Scenario final : public obd::CanTransmitter, public obd::SignalRecorder {
 public:
  Scenario(int index, std::int64_t wallBase, std::int64_t endMs, bool busIsActive, bool serviceEnabled,
           std::string directory)
      : index_(index),
        wallBase_(wallBase),
        endMs_(endMs),
        displayDirectory_(std::move(directory)),
        loop_(order_),
        obd_(*this, *this, false, order_) {
    char pattern[] = "/tmp/emcool-service-XXXXXX";
    directory_ = ::mkdtemp(pattern);
    store_ = std::make_unique<VcuStore>(directory_, names_);
    context_.send = [this](std::uint32_t id, const std::array<std::uint8_t, 8>& frame) {
      return transmit(id, frame);
    };
    context_.readSignal = [this](std::string_view key) {
      const auto found = signals_.find(std::string(key));
      if (found == signals_.end()) return GateSample{};
      return GateSample{found->second.first, static_cast<double>(nowMs_ - found->second.second)};
    };
    context_.wallClockMs = [this] { return static_cast<double>(wallBase_ + nowMs_); };
    context_.hold = &obd_.hold();
    context_.store = store_.get();
    runner_ = std::make_unique<VcuReadRunner>(context_, lease_, busIsActive);
    readings_ = std::make_unique<ReadingStore>(directory_);
    api_ = std::make_unique<ServiceApi>(*runner_, *store_, *readings_, serviceEnabled,
                                        [this] { return static_cast<double>(wallBase_ + nowMs_); });
  }

  ~Scenario() override { std::filesystem::remove_all(directory_); }

  std::map<std::int64_t, Millisecond>& timeline() { return timeline_; }
  // An earlier sweep's resume file, there before the run starts.
  void seedPartialFile(const std::string& content) {
    std::FILE* file = std::fopen((directory_ + "/sweep.partial.jsonl").c_str(), "wb");
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
  }
  int mismatches() const { return mismatches_; }

  std::optional<std::string> transmit(std::uint32_t id, const obd::Payload& data) override {
    char text[64];
    std::snprintf(text, sizeof text, "t %x %s", static_cast<unsigned>(id), toHexLower(data).c_str());
    for (Expected& expected : bucket().outputs) {
      if (!expected.consumed && expected.text == text) {
        expected.consumed = true;
        if (expected.fail) return std::string("simulated send failure");
        return std::nullopt;
      }
    }
    unexpected(text);
    return std::nullopt;
  }

  void record(std::string_view key, double value) override {
    signals_[std::string(key)] = {value, nowMs_};
    produced("s " + std::string(key) + " " + formatValue(value));
  }

  void logged(char level, const char*, const char* message) {
    std::string text = translate(message);
    for (Expected& expected : bucket().outputs) {
      if (expected.consumed || expected.text.compare(0, 2, "l ") != 0) continue;
      const std::string wantedLevel = expected.text.substr(2, 1);
      const std::string wanted = messageOf(expected.text.substr(4));
      if (wantedLevel[0] != level) continue;
      if (wanted == text || (level == 'E' && text.compare(0, wanted.size(), wanted) == 0)) {
        expected.consumed = true;
        return;
      }
    }
    unexpected(std::string("l ") + level + " " + text);
  }

  void run() {
    async::setCurrentLoop(&loop_);
    nowMs_ = 0;
    loop_.run(0, [&] { obd_.start(now()); });
    finishMillisecond();
    while (true) {
      std::optional<std::int64_t> next;
      const auto consider = [&](std::optional<std::int64_t> candidate) {
        if (candidate && (!next || *candidate < *next)) next = candidate;
      };
      if (const auto timer = loop_.nextTimer()) consider(timer->atMs);
      if (const auto deadline = obd_.nextDeadline()) consider(deadline->atMs);
      const auto upcoming = timeline_.upper_bound(nowMs_);
      if (upcoming != timeline_.end()) consider(upcoming->first);
      if (!next) break;
      if (*next <= nowMs_) {
        mismatch(nowMs_, "a deadline did not move on after firing");
        next = nowMs_ + 1;
      }
      nowMs_ = *next;
      if (nowMs_ > endMs_) break;
      Millisecond& here = bucket();
      for (const std::string& input : here.inputs) loop_.run(nowMs_, [&] { apply(input); });
      fireDue();
      for (const auto& [id, data] : here.arrivals) loop_.run(nowMs_, [&] { route(id, data); });
      finishMillisecond();
    }
    for (auto& [ms, bucket] : timeline_) {
      for (const Expected& expected : bucket.outputs) {
        if (!expected.consumed) mismatch(ms, "missing: " + expected.text);
      }
    }
    async::setCurrentLoop(nullptr);
  }

 private:
  obd::Now now() const { return {nowMs_, wallBase_ + nowMs_}; }
  Millisecond& bucket() { return timeline_[nowMs_]; }

  std::string translate(std::string text) const {
    text = replaceAll(text, directory_, displayDirectory_);
    text = replaceAll(text, ".jsonl", ".json");
    return replaceAll(text, "latest.txt", "latest.json");
  }

  // The loop's timers and the OBD side's deadlines, together, in the order they were set.
  void fireDue() {
    for (int fired = 0; fired < 10000; fired++) {
      const std::optional<obd::Deadline> timer = loop_.nextTimer();
      const std::optional<obd::Deadline> deadline = obd_.nextDeadline();
      const std::optional<obd::Deadline> next = obd::earliest(timer, deadline);
      if (!next || next->atMs > nowMs_) return;
      if (timer && timer->order == next->order) {
        loop_.fireNext(nowMs_);
      } else {
        loop_.run(nowMs_, [&] { obd_.fireNext(now()); });
      }
    }
    mismatch(nowMs_, "timers kept firing");
  }

  void route(std::uint32_t id, const std::vector<std::uint8_t>& data) {
    if (obd::ObdPoller::isResponse(id)) {
      if (runner_->handleCanFrame(id, data, std::nullopt)) return;
      obd_.handleFrame(id, data, now());
    } else if (obd::ElockReader::isResponse(id)) {
      obd_.handleFrame(id, data, now());
    }
  }

  void answer(const std::string& what, const std::string& json) { produced("j " + what + " " + translate(json)); }

  void apply(const std::string& input) {
    if (input.rfind("g ", 0) == 0) {
      const std::vector<std::string> words = fields(input, 3);
      signals_[words[1]] = {std::strtod(words[2].c_str(), nullptr), nowMs_};
      return;
    }
    const std::string action = input.substr(2);
    if (action == "start") {
      const VcuReadRunner::Started started = runner_->start();
      answer("start", build([&](text::TextBuffer& out) { writeStartedJson(out, started); }));
    } else if (action == "cancel") {
      answer("cancel", runner_->cancel() ? "true" : "false");
    } else if (action == "state") {
      answer("state", build([&](text::TextBuffer& out) { runner_->writeStateJson(out); }));
    } else if (action == "gate") {
      answer("gate", build([&](text::TextBuffer& out) { writeVerdictJson(out, runner_->gate()); }));
    } else if (action == "params") {
      ParamsBody body(*store_, directory_);
      answer("params", wholeBody(body));
    } else if (action == "backup") {
      BackupBody body(*store_, directory_);
      std::string json;
      if (body.status() == 200 || body.status() == 409) {
        const std::string csv = body.status() == 200 ? wholeBody(body) : backupCsvLine(nullptr);
        json = build([&](text::TextBuffer& out) {
          out.append("{\"rows\":");
          out.appendInteger(body.exportableRows());
          out.append(",\"csv\":");
          out.appendJsonString(csv);
          out.append("}");
        });
      } else {
        json = body.status() == 404 ? "{\"state\":\"never-read\"}" : "{\"state\":\"unreadable\"}";
      }
      answer("backup", json);
    } else if (action.rfind("probe ", 0) == 0) {
      const std::vector<std::string> words = fields(action, 4);
      const ProbeParse parsed = parseProbeRequest(words[1], words[2], words[3]);
      if (!parsed.request) {
        answer("probe", build([&](text::TextBuffer& out) {
          out.append("{\"ok\":false,\"reason\":");
          out.appendJsonString(parsed.reason);
          out.append("}");
        }));
        return;
      }
      [](Scenario* scenario, VcuProbeRequest request) -> async::Task<> {
        const ProbeOutcome outcome = co_await scenario->runner_->probe(request);
        scenario->answer("probe", build([&](text::TextBuffer& out) { writeProbeOutcomeJson(out, outcome); }));
      }(this, *parsed.request).detach();
    } else if (action == "lifetime" || action == "freeze" || action == "lifetime-stats" || action == "freeze-frames" ||
               action == "expected-get" || action.rfind("expected-set ", 0) == 0) {
      ServiceRequest request{ServiceRequest::Kind::kLifetimeStats, {}, {}, {}, {}, {}, {}, {}};
      std::string what = action;
      if (action == "lifetime") request.kind = ServiceRequest::Kind::kLifetimeRead;
      if (action == "freeze") request.kind = ServiceRequest::Kind::kFreezeFrameRead;
      if (action == "freeze-frames") request.kind = ServiceRequest::Kind::kFreezeFrames;
      if (action == "expected-get") {
        request.kind = ServiceRequest::Kind::kExpectedFaults;
        what = "expected";
      }
      if (action.rfind("expected-set ", 0) == 0) {
        const std::vector<std::string> words = fields(action, 4);
        request = {ServiceRequest::Kind::kSetExpectedFault, {}, {}, {}, words[1], words[2], words[3], {}};
        what = "expected";
      }
      [](Scenario* scenario, ServiceRequest asked, std::string label) -> async::Task<> {
        const ServiceAnswer answered = co_await scenario->api_->answer(std::move(asked));
        scenario->answer(label, std::to_string(answered.status) + " " + answered.body);
      }(this, request, what)
          .detach();
    } else if (action.rfind("expire ", 0) == 0) {
      // The simulated micro's, not ours.
    } else {
      mismatch(nowMs_, "unknown action " + action);
    }
  }

  void produced(const std::string& text) {
    for (Expected& expected : bucket().outputs) {
      if (!expected.consumed && expected.text == text) {
        expected.consumed = true;
        return;
      }
    }
    unexpected(text);
  }

  void unexpected(const std::string& text) { mismatch(nowMs_, "unexpected: " + text); }

  void finishMillisecond() {
    for (Expected& expected : bucket().outputs) {
      if (!expected.consumed) mismatch(nowMs_, "missing: " + expected.text);
      expected.consumed = true;
    }
  }

  void mismatch(std::int64_t ms, const std::string& what) {
    if (mismatches_++ < 12) {
      std::fprintf(stderr, "run %d at %" PRId64 " ms: %s\n", index_, ms, what.substr(0, 700).c_str());
    }
  }

  int index_;
  std::int64_t wallBase_;
  std::int64_t endMs_;
  std::string displayDirectory_;
  std::string directory_;
  obd::DeadlineOrder order_;
  async::Loop loop_;
  obd::ObdStack obd_;
  ParameterNames names_;
  std::unique_ptr<VcuStore> store_;
  ServiceContext context_;
  BusLease lease_;
  std::unique_ptr<VcuReadRunner> runner_;
  std::unique_ptr<ReadingStore> readings_;
  std::unique_ptr<ServiceApi> api_;
  std::map<std::string, std::pair<double, std::int64_t>> signals_;
  std::map<std::int64_t, Millisecond> timeline_;
  std::int64_t nowMs_ = 0;
  int mismatches_ = 0;
};

Scenario* activeScenario = nullptr;

void observeLog(char level, const char* tag, const char* message, void*) {
  if (activeScenario != nullptr) activeScenario->logged(level, tag, message);
}

void loadScenario(std::istream& input, Scenario& scenario, int& frames) {
  std::string line;
  while (std::getline(input, line) && line != "end") {
    if (line.rfind("seed ", 0) == 0) {
      emcool::ble::JsonObjectReader reader;
      const std::string wrapped = "{\"seed\":" + line.substr(5) + "}";
      std::vector<char> scratch(wrapped.size() + 16);
      if (reader.read(wrapped, scratch)) scenario.seedPartialFile(std::string(reader.find("seed")->text));
      continue;
    }
    const std::vector<std::string> words = fields(line, 3);
    const std::int64_t ms = std::stoll(words[1]);
    Millisecond& bucket = scenario.timeline()[ms];
    const std::string rest = words[2];
    switch (line[0]) {
      case 'g':
        bucket.inputs.push_back("g " + rest);
        break;
      case 'a':
        bucket.inputs.push_back("a " + rest);
        break;
      case 'r': {
        const std::vector<std::string> parts = fields(rest, 2);
        bucket.arrivals.emplace_back(static_cast<std::uint32_t>(std::stoul(parts[0], nullptr, 16)), parseHex(parts[1]));
        break;
      }
      case 't': {
        const std::vector<std::string> parts = fields(rest, 3);
        bucket.outputs.push_back({"t " + parts[0] + " " + parts[1], parts[2] == "fail", false});
        frames++;
        break;
      }
      case 's': {
        const std::vector<std::string> parts = fields(rest, 2);
        bucket.outputs.push_back({"s " + parts[0] + " " + formatValue(std::strtod(parts[1].c_str(), nullptr)), false, false});
        break;
      }
      case 'l':
      case 'j':
        bucket.outputs.push_back({std::string(1, line[0]) + " " + emcoolWording(rest), false, false});
        break;
      default:
        std::fprintf(stderr, "check_service: unknown line %s\n", line.substr(0, 80).c_str());
        std::exit(2);
    }
  }
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_service <golden file>\n");
    return 2;
  }
  std::ifstream input(arguments[1]);
  setLogObserver(observeLog, nullptr);
  int scenarios = 0;
  int frames = 0;
  int mismatches = 0;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("run ", 0) != 0) continue;
    const std::vector<std::string> words = fields(line, 7);
    auto scenario = std::make_unique<Scenario>(std::stoi(words[1]), std::stoll(words[2]), std::stoll(words[3]),
                                               words[4] == "1", words[5] == "1", words[6]);
    loadScenario(input, *scenario, frames);
    activeScenario = scenario.get();
    scenario->run();
    activeScenario = nullptr;
    scenarios++;
    if (scenario->mismatches() > 0) {
      std::fprintf(stderr, "run %s: %d mismatches\n", words[1].c_str(), scenario->mismatches());
      mismatches += scenario->mismatches();
    }
  }
  setLogObserver(nullptr, nullptr);
  EMCOOL_CHECK(mismatches == 0, "%d mismatches", mismatches);
  std::printf("check_service: %d scenarios, %d frames sent; %d failures\n", scenarios, frames,
              emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
