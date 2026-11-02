// Holds the C++ diagnostic decoders to cool-eva's, line by line of
// tools/generate-golden-diagnostics.mts' output: freeze frames, the lifetime statistics, the 0x18
// stored-code list, and the hub's active fault list with every signal it records and line it logs.
//
//   check_diagnostics <golden file>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/obd/freeze_frame.hpp"
#include "emcool/obd/hub_diagnostics.hpp"
#include "emcool/obd/lifetime.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/readings.hpp"

using namespace emcool;

namespace {

std::vector<std::uint8_t> parseHex(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(at, 2)), nullptr, 16)));
  }
  return bytes;
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

template <typename Write>
std::string build(Write write) {
  std::string text(256 * 1024, '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

void compare(const char* what, const std::string& input, const std::string& mine, const std::string& theirs) {
  EMCOOL_CHECK(mine == theirs, "%s %s:\n  emcool   %s\n  cool-eva %s", what, input.substr(0, 80).c_str(),
               mine.substr(0, 1200).c_str(), theirs.substr(0, 1200).c_str());
}

std::string formatValue(double value) {
  char text[40];
  std::snprintf(text, sizeof text, "%.17g", value);
  return text;
}

// The hub's records and log lines, in the order they happened.
std::vector<std::string> produced;

class Recorder final : public obd::SignalRecorder {
 public:
  void record(std::string_view key, double value) override {
    produced.push_back("HS " + std::string(key) + " " + formatValue(value));
  }
};

void observeLog(char level, const char* tag, const char* message, void*) {
  produced.push_back(std::string("HL ") + level + " " + tag + ": " + message);
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_diagnostics <golden file>\n");
    return 2;
  }
  std::ifstream input(arguments[1]);
  int frames = 0, summaries = 0, lists = 0, hubFrames = 0;
  std::vector<std::string> expectedValues;
  obd::FreezeFrameResponse lastResponse;
  std::unique_ptr<obd::HubDiagnostics> hub;
  Recorder recorder;
  std::vector<std::string> expectedHub;
  std::string line;
  const auto flushHub = [&] {
    std::string mine, theirs;
    for (const std::string& item : produced) mine += item + "\n";
    for (std::string item : expectedHub) {
      if (item.rfind("HS ", 0) == 0) {
        const std::vector<std::string> words = fields(item, 3);
        item = "HS " + words[1] + " " + formatValue(std::strtod(words[2].c_str(), nullptr));
      }
      theirs += item + "\n";
    }
    compare("hub diagnostics", "", mine, theirs);
    produced.clear();
    expectedHub.clear();
  };
  while (std::getline(input, line)) {
    const std::string kind = line.substr(0, line.find(' '));
    if (kind == "FF") {
      const std::vector<std::string> words = fields(line, 5);
      const std::vector<std::uint8_t> payload = parseHex(words[2]);
      lastResponse = obd::decodeFreezeFrameResponse(payload, std::stoi(words[1]));
      compare("isNoStoredRecordReply", words[2], obd::isNoStoredRecordReply(payload) ? "1" : "0", words[3]);
      compare("decodeFreezeFrameResponse", words[2],
              build([&](text::TextBuffer& out) { obd::writeFreezeFrameResponseJson(out, lastResponse); }), words[4]);
      expectedValues.clear();
      frames++;
    } else if (kind == "FV") {
      expectedValues.push_back(line.substr(3));
    } else if (kind == "FE") {
      std::string mine, theirs;
      if (lastResponse.kind == obd::FreezeFrameResponse::Kind::kFrame) {
        for (const obd::FreezeFrameValue& value : lastResponse.frame.values) {
          mine += build([&](text::TextBuffer& out) { out.appendJsonString(obd::formatFreezeFrameValue(value)); }) + "\n";
        }
      }
      for (const std::string& value : expectedValues) theirs += value + "\n";
      compare("formatFreezeFrameValue", "", mine, theirs);
    } else if (kind == "LT") {
      // The entries' JSON holds spaces of its own; it ends where the statistics begin.
      const std::vector<std::string> head = fields(line, 3);
      const std::size_t boundary = head[2].find("] {\"readAt\"");
      const std::vector<std::string> words = {head[0], head[1], head[2].substr(0, boundary + 1),
                                              head[2].substr(boundary + 2)};
      std::vector<std::string_view> elements;
      EMCOOL_CHECK(ble::jsonArrayElements(words[2], elements), "unreadable entries %s", words[2].c_str());
      std::vector<obd::ComponentResponse> responses;
      for (const std::string_view element : elements) {
        ble::JsonObjectReader reader;
        std::vector<char> scratch(element.size() + 16);
        EMCOOL_CHECK(reader.read(element, scratch), "unreadable entry");
        const int component = static_cast<int>(reader.find("component")->number);
        const ble::JsonMember* hex = reader.find("hex");
        const ble::JsonMember* reason = reader.find("reason");
        const std::string hexText = hex->type == ble::JsonType::kString ? std::string(hex->text) : "";
        if (reason->type == ble::JsonType::kString) {
          responses.push_back({component, obd::unrecognisedResponse(std::string(reason->text),
                                                                    hex->type == ble::JsonType::kString ? hexText : "")});
        } else {
          responses.push_back({component, obd::decodeFreezeFrameResponse(parseHex(hexText), component)});
        }
      }
      const obd::LifetimeStatistics statistics = obd::summariseLifetimeStatistics(std::stod(words[1]), responses);
      compare("summariseLifetimeStatistics", words[2],
              build([&](text::TextBuffer& out) { obd::writeLifetimeStatisticsJson(out, statistics); }), words[3]);
      summaries++;
    } else if (kind == "SL") {
      const std::vector<std::string> words = fields(line, 2);
      const std::string rest = words[1];
      const std::size_t space = rest.find(' ');
      const std::vector<std::uint8_t> body = parseHex(rest.substr(0, space));
      const vcu::VcuStoredDtcList stored = vcu::decodeStoredDtcList(body);
      const vcu::ComponentSelection selection = vcu::selectComponentsToRead(stored);
      const std::optional<std::string> damage = vcu::describeListDamage(selection.report);
      const std::string mine = build([&](text::TextBuffer& out) {
        out.append("{\"declaredCount\":");
        out.appendInteger(stored.declaredCount);
        out.append(",\"records\":[");
        for (std::size_t at = 0; at < stored.records.size(); at++) {
          if (at > 0) out.append(",");
          out.append("{\"code\":");
          out.appendInteger(stored.records[at].code);
          out.append(",\"status\":");
          out.appendInteger(stored.records[at].status);
          out.append("}");
        }
        out.append("],\"paddingRecords\":");
        out.appendInteger(stored.paddingRecords);
        out.append(",\"trailingHex\":");
        out.appendJsonString(stored.trailingHex);
        out.append(stored.truncated ? ",\"truncated\":true} {\"components\":[" : ",\"truncated\":false} {\"components\":[");
        for (std::size_t at = 0; at < selection.components.size(); at++) {
          if (at > 0) out.append(",");
          out.appendInteger(selection.components[at]);
        }
        out.append("],\"report\":");
        vcu::writeListReportJson(out, selection.report);
        out.append("} ");
        if (damage) {
          out.appendJsonString(*damage);
        } else {
          out.append("null");
        }
      });
      compare("stored code list", rest.substr(0, space), mine, rest.substr(space + 1));
      lists++;
    } else if (kind == "HI") {
      hub = std::make_unique<obd::HubDiagnostics>();
      setLogObserver(observeLog, nullptr);
    } else if (kind == "HF") {
      flushHub();
      const std::vector<std::uint8_t> frame = parseHex(line.substr(3));
      hub->handle(frame, recorder);
      hubFrames++;
      // Its effects come next in the file; compare them at the next frame.
      expectedHub.clear();
    } else if (kind == "HS" || kind == "HL") {
      expectedHub.push_back(line);
    } else if (kind == "HE") {
      flushHub();
      setLogObserver(nullptr, nullptr);
    } else {
      std::fprintf(stderr, "check_diagnostics: unknown line %s\n", line.substr(0, 60).c_str());
      return 2;
    }
  }
  std::printf("check_diagnostics: %d freeze-frame replies, %d lifetime summaries, %d code lists, %d hub frames; %d "
              "failures\n",
              frames, summaries, lists, hubFrames, emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
