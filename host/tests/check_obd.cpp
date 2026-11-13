// Replays tools/generate-golden-obd.mts' output through the C++ OBD port: the pure functions
// line for line, and each scenario millisecond by millisecond — the same frames arrive and
// the same holds are asked for at the same moments, and in every millisecond the C++ must
// send the same frames, record the same signals, log the same lines and serve the same
// /stored-dtcs body as the reference did. Within one millisecond the order isn't compared (the
// E-LOCK read and the poller run side by side), only what happened.
//
//   check_obd <golden file>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/log.hpp"
#include "emcool/obd/dtc_table.hpp"
#include "emcool/obd/iso_tp.hpp"
#include "emcool/obd/obd_dtc.hpp"
#include "emcool/obd/obd_stack.hpp"
#include "emcool/text/text_buffer.hpp"

using namespace emcool::obd;

namespace {

// The reason the reference's transmitCanFrame gives for a send() that returned -1, which reaches
// /stored-dtcs and the logs; emcool's own reason is whatever its bus says.
std::string referenceRefusal(std::uint32_t id) {
  char text[128];
  std::snprintf(text, sizeof text, "CAN 0x%x not sent: send() returned -1, expected 16 (socketcan does not expose errno)",
                static_cast<unsigned>(id));
  return text;
}

std::vector<std::uint8_t> parseHex(const std::string& text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(at, 2).c_str(), nullptr, 16)));
  }
  return bytes;
}

std::string toHex(std::span<const std::uint8_t> bytes) {
  std::string text;
  char pair[3];
  for (const std::uint8_t byte : bytes) {
    std::snprintf(pair, sizeof pair, "%02x", byte);
    text += pair;
  }
  return text.empty() ? "-" : text;
}

// The words of a line, the last `restFrom`-th onwards kept whole (it may hold spaces).
std::vector<std::string> fields(const std::string& line, std::size_t count) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (out.size() + 1 < count && at <= line.size()) {
    const std::size_t space = line.find(' ', at);
    if (space == std::string::npos) break;
    out.push_back(line.substr(at, space - at));
    at = space + 1;
  }
  out.push_back(at <= line.size() ? line.substr(at) : "");
  return out;
}

const char* statusName(IsoTpStatus status) {
  switch (status) {
    case IsoTpStatus::kIncomplete:
      return "incomplete";
    case IsoTpStatus::kFlowControlRequired:
      return "flow-control-required";
    case IsoTpStatus::kComplete:
      return "complete";
    case IsoTpStatus::kIgnored:
      return "ignored";
    case IsoTpStatus::kAbandoned:
      return "abandoned";
  }
  return "?";
}

std::string formatValue(double value) {
  char text[40];
  std::snprintf(text, sizeof text, "%.17g", value);
  return text;
}

// ---------------------------------------------------------------------------------------------
// One scenario.

struct Expected {
  std::string text;
  bool fail = false;  // a sent frame whose send the reference saw throw
  bool consumed = false;
};

struct Millisecond {
  std::vector<std::string> actions;  // in order: "hold <wait> <max> <reason>", "release", "snapshot"
  std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> arrivals;
  std::vector<Expected> outputs;
};

class Scenario final : public CanTransmitter, public SignalRecorder {
 public:
  Scenario(int index, bool elock, std::int64_t wallBase, std::int64_t endMs)
      : index_(index), wallBase_(wallBase), endMs_(endMs), stack_(*this, *this, elock) {}

  std::map<std::int64_t, Millisecond>& timeline() { return timeline_; }

  std::optional<std::string> transmit(std::uint32_t id, const Payload& data) override {
    char text[64];
    std::snprintf(text, sizeof text, "t %x %s", static_cast<unsigned>(id), toHex(data).c_str());
    // Fails exactly the sends the reference's did.
    for (Expected& expected : bucket().outputs) {
      if (!expected.consumed && expected.text == text) {
        expected.consumed = true;
        if (expected.fail) return referenceRefusal(id);
        return std::nullopt;
      }
    }
    unexpected(text);
    return std::nullopt;
  }

  void record(std::string_view key, double value) override {
    produced("s " + std::string(key) + " " + formatValue(value));
  }

  void logged(char level, const char* tag, const char* message) {
    const std::string text = std::string(1, level) + " " + tag + ": " + message;
    // The reference logs a send failure's error as a second argument, which the golden file leaves
    // out; emcool puts the reason on the same line.
    for (Expected& expected : bucket().outputs) {
      if (expected.consumed || expected.text.compare(0, 2, "l ") != 0) continue;
      const std::string wanted = expected.text.substr(2);
      if (wanted == text || (level == 'E' && text.compare(0, wanted.size(), wanted) == 0)) {
        expected.consumed = true;
        return;
      }
    }
    unexpected("l " + text);
  }

  void run() {
    nowMs_ = 0;
    stack_.start(now());
    finishMillisecond();
    while (true) {
      std::optional<std::int64_t> next = stack_.nextDeadlineMs();
      const auto upcoming = timeline_.upper_bound(nowMs_);
      if (upcoming != timeline_.end() && (!next || upcoming->first < *next)) next = upcoming->first;
      if (!next) break;
      if (*next <= nowMs_) {
        // A deadline still due after its tick: report it and move on, rather than spin.
        mismatch(nowMs_, "a deadline did not move on after firing");
        next = nowMs_ + 1;
      }
      nowMs_ = *next;
      if (nowMs_ > endMs_ + 1) break;
      Millisecond& here = bucket();
      resolveHold();
      for (const std::string& action : here.actions) act(action);
      if (nowMs_ <= endMs_) {
        stack_.tick(now());
        for (const auto& [id, data] : here.arrivals) stack_.handleFrame(id, data, now());
      }
      finishMillisecond();
    }
    for (auto& [ms, bucket] : timeline_) {
      for (const Expected& expected : bucket.outputs) {
        if (!expected.consumed) mismatch(ms, "missing: " + expected.text);
      }
    }
  }

  int mismatches() const { return mismatches_; }

 private:
  Now now() const { return {nowMs_, wallBase_ + nowMs_}; }
  Millisecond& bucket() { return timeline_[nowMs_]; }

  // The scenario's `current`: the handle holdObdPoller()'s promise last resolved to — a hold
  // once the poller parks for it, null once one is refused — which is what a release releases.
  void resolveHold() {
    if (!pending_) return;
    const HoldStatus status = stack_.hold().status(*pending_);
    if (status == HoldStatus::kWaiting) return;
    current_ = status == HoldStatus::kRefused ? std::nullopt : pending_;
    pending_.reset();
  }

  void act(const std::string& action) {
    resolveHold();
    if (action.rfind("hold ", 0) == 0) {
      const std::vector<std::string> words = fields(action, 4);
      const std::int64_t waitMs = std::stoll(words[1]);
      const std::int64_t maxHoldMs = std::stoll(words[2]);
      if (const std::optional<HoldTicket> ticket = stack_.hold().request(words[3], nowMs_, waitMs, maxHoldMs)) {
        pending_ = ticket;
      } else {
        current_.reset();
      }
    } else if (action == "release") {
      if (current_) {
        stack_.hold().release(*current_);
      } else {
        mismatch(nowMs_, "the reference released a hold the C++ never granted");
      }
      current_.reset();
    } else if (action == "snapshot") {
      std::string body(256 * 1024, '\0');
      emcool::text::TextBuffer out(body.data(), body.size());
      stack_.stored().writeJson(out, nowMs_);
      EMCOOL_CHECK(!out.overflowed(), "run %d: the snapshot overflowed", index_);
      body.resize(out.length());
      // The same body, a window at a time, as the Bluetooth server sends it.
      std::string pieces;
      for (std::size_t skip = 0;; skip += 1000) {
        char piece[1000];
        emcool::text::TextBuffer window = emcool::text::TextBuffer::window(piece, sizeof piece, skip);
        stack_.stored().writeJson(window, nowMs_);
        pieces.append(piece, window.length());
        if (window.total() != body.size()) mismatch(nowMs_, "a window's total differs from the body's length");
        if (skip + window.length() >= window.total()) break;
      }
      if (pieces != body) mismatch(nowMs_, "the body read in windows differs from the whole");
      produced("j " + body);
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
    for (const Expected& expected : bucket().outputs) {
      if (!expected.consumed) {
        mismatch(nowMs_, "missing: " + expected.text);
      }
    }
    for (Expected& expected : bucket().outputs) expected.consumed = true;
  }

  void mismatch(std::int64_t ms, const std::string& what) {
    if (mismatches_++ < 12) {
      std::fprintf(stderr, "run %d at %" PRId64 " ms: %s\n", index_, ms, what.substr(0, 400).c_str());
    }
  }

  int index_;
  std::int64_t wallBase_;
  std::int64_t endMs_;
  ObdStack stack_;
  std::map<std::int64_t, Millisecond> timeline_;
  std::int64_t nowMs_ = 0;
  std::optional<HoldTicket> pending_;
  std::optional<HoldTicket> current_;
  int mismatches_ = 0;
};

Scenario* activeScenario = nullptr;

void observeLog(char level, const char* tag, const char* message, void*) {
  if (activeScenario != nullptr) activeScenario->logged(level, tag, message);
}

// ---------------------------------------------------------------------------------------------

struct Totals {
  int codes = 0;
  int responseCodes = 0;
  int isoPushes = 0;
  int decoded = 0;
  int scenarios = 0;
  int scenarioMismatches = 0;
  int frames = 0;
};

void checkCode(const std::string& line, Totals& totals) {
  const std::vector<std::string> words = fields(line, 5);
  const auto raw = static_cast<std::uint16_t>(std::stoul(words[1]));
  const ObdCodeText code = formatObdDtc(raw);
  const DtcTableEntry* entry = lookupByObdCode(code.view());
  const int component = entry ? entry->component : -1;
  const int symptom = entry ? entry->symptom : -1;
  EMCOOL_CHECK(code.view() == words[2] && component == std::stoi(words[3]) && symptom == std::stoi(words[4]),
               "0x%04x: %s %d/%d, the reference %s %s/%s", raw, code.characters.data(), component, symptom, words[2].c_str(),
               words[3].c_str(), words[4].c_str());
  totals.codes++;
}

void checkIsoTpPush(IsoTpReassembler& reassembler, const std::string& line, Totals& totals) {
  const std::vector<std::string> words = fields(line, 6);
  const std::vector<std::uint8_t> frame = parseHex(words[1]);
  const IsoTpResult result = reassembler.push(frame);
  const std::string payload = result.status == IsoTpStatus::kComplete ? toHex(result.payload) : "-";
  const std::string reason = result.reason.empty() ? "-" : result.reason;
  const std::string total = std::to_string(result.totalLength);
  EMCOOL_CHECK(words[2] == statusName(result.status) && words[3] == total && words[4] == payload && words[5] == reason,
               "ISO-TP push %s: %s %s %s %s, the reference %s %s %s %s", words[1].c_str(), statusName(result.status),
               total.c_str(), payload.c_str(), reason.c_str(), words[2].c_str(), words[3].c_str(), words[4].c_str(),
               words[5].c_str());
  totals.isoPushes++;
}

void checkDecode(const std::string& line, Totals& totals) {
  const std::vector<std::string> words = fields(line, 11);
  const int mode = std::stoi(words[1]);
  const ObdDtcResponse response = decodeObdDtcResponse(parseHex(words[2]), mode);
  std::string mine;
  if (response.kind == ObdDtcResponse::Kind::kCodes) {
    std::string codes;
    for (const std::uint16_t raw : response.codes) {
      if (!codes.empty()) codes += ",";
      char text[8];
      std::snprintf(text, sizeof text, "%x", raw);
      codes += text;
    }
    mine = std::string("codes ") + listName(response.list) + " " + std::to_string(response.declaredCount) + " " +
           (codes.empty() ? "-" : codes) + " " + (response.truncated ? "1" : "0") + " 0 0 -";
  } else if (response.kind == ObdDtcResponse::Kind::kNegative) {
    mine = "negative - 0 - 0 " + std::to_string(response.requestedService) + " " +
           std::to_string(response.negativeResponseCode) + " -";
  } else {
    mine = "unrecognised - 0 - 0 0 0 " + response.reason;
  }
  const std::string theirs = line.substr(line.find(' ', line.find(' ', 2) + 1) + 1);
  EMCOOL_CHECK(mine == theirs, "decode mode 0x%x %s: %s, the reference %s", mode, words[2].c_str(), mine.c_str(),
               theirs.c_str());
  totals.decoded++;
}

// Reads one scenario's lines into its timeline.
void loadScenario(std::istream& input, Scenario& scenario, Totals& totals) {
  std::string line;
  while (std::getline(input, line) && line != "end") {
    const std::vector<std::string> words = fields(line, 3);
    const std::int64_t ms = std::stoll(words[1]);
    Millisecond& bucket = scenario.timeline()[ms];
    const std::string rest = words[2];
    switch (line[0]) {
      case 'a':
        bucket.actions.push_back(rest);
        break;
      case 'r': {
        const std::vector<std::string> parts = fields(rest, 2);
        bucket.arrivals.emplace_back(static_cast<std::uint32_t>(std::stoul(parts[0], nullptr, 16)), parseHex(parts[1]));
        break;
      }
      case 't': {
        const std::vector<std::string> parts = fields(rest, 3);
        bucket.outputs.push_back({"t " + parts[0] + " " + parts[1], parts[2] == "fail", false});
        totals.frames++;
        break;
      }
      case 's': {
        const std::vector<std::string> parts = fields(rest, 2);
        bucket.outputs.push_back({"s " + parts[0] + " " + formatValue(std::strtod(parts[1].c_str(), nullptr)), false, false});
        break;
      }
      case 'l':
      case 'j':
        bucket.outputs.push_back({std::string(1, line[0]) + " " + rest, false, false});
        break;
      default:
        std::fprintf(stderr, "check_obd: unknown line %s\n", line.substr(0, 80).c_str());
        std::exit(2);
    }
  }
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_obd <golden file>\n");
    return 2;
  }
  std::ifstream input(arguments[1]);
  if (!input) {
    std::fprintf(stderr, "check_obd: cannot read %s\n", arguments[1]);
    return 2;
  }
  emcool::setLogObserver(observeLog, nullptr);
  Totals totals;
  IsoTpReassembler reassembler;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    if (line.rfind("F ", 0) == 0) {
      checkCode(line, totals);
    } else if (line.rfind("N ", 0) == 0) {
      const std::vector<std::string> words = fields(line, 3);
      const std::string mine = describeNegativeResponseCode(std::stoi(words[1]));
      EMCOOL_CHECK(mine == words[2], "NRC %s: %s, the reference %s", words[1].c_str(), mine.c_str(), words[2].c_str());
      totals.responseCodes++;
    } else if (line == "I") {
      reassembler = IsoTpReassembler();
    } else if (line.rfind("P ", 0) == 0) {
      checkIsoTpPush(reassembler, line, totals);
    } else if (line.rfind("D ", 0) == 0) {
      checkDecode(line, totals);
    } else if (line.rfind("run ", 0) == 0) {
      const std::vector<std::string> words = fields(line, 5);
      auto scenario = std::make_unique<Scenario>(std::stoi(words[1]), words[2] == "1", std::stoll(words[3]),
                                                 std::stoll(words[4]));
      loadScenario(input, *scenario, totals);
      activeScenario = scenario.get();
      scenario->run();
      activeScenario = nullptr;
      totals.scenarios++;
      if (scenario->mismatches() > 0) {
        std::fprintf(stderr, "run %s: %d mismatches\n", words[1].c_str(), scenario->mismatches());
        totals.scenarioMismatches += scenario->mismatches();
      }
    } else {
      std::fprintf(stderr, "check_obd: unknown line %s\n", line.substr(0, 80).c_str());
      return 2;
    }
  }
  emcool::setLogObserver(nullptr, nullptr);
  EMCOOL_CHECK(totals.scenarioMismatches == 0, "%d scenario mismatches", totals.scenarioMismatches);
  std::printf(
      "check_obd: %d codes formatted, %d NRCs, %d ISO-TP pushes, %d replies decoded; %d scenarios, %d frames sent; "
      "%d failures\n",
      totals.codes, totals.responseCodes, totals.isoPushes, totals.decoded, totals.scenarios, totals.frames,
      emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
