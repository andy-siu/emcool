// Holds the C++ VCU port to the reference implementation's own code, line by line of tools/generate-golden-vcu.mts'
// output: the same inputs through the C++, and the same JSON (or text) out.
//
// Two of the reference's messages point at its own extract script; emcool's point at
// docs/service-mode.md and the generator. Those sentences are swapped in the expected text before
// comparing.
//
//   check_vcu <golden file>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/obd/obd_dtc.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/service_gate.hpp"
#include "emcool/vcu/snapshot.hpp"

using namespace emcool::vcu;
using emcool::text::TextBuffer;

namespace {

std::vector<std::uint8_t> parseHex(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(at, 2)), nullptr, 16)));
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
  return text.empty() ? "-" : text;
}

// The first `count - 1` space-separated words, then the rest whole.
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

std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

// The reference's two pointers at its extract script, as emcool words them (JSON-escaped as in the file).
std::string emcoolWording(std::string text) {
  text = replaceAll(text,
                    "Add the table with scripts/extract-vcu-tables.ts (README: \\\"Adding your bike's VCU parameter "
                    "table\\\").",
                    "Add the table as docs/service-mode.md (\\\"Parameter tables\\\") describes and run "
                    "tools/generate-vcu-tables.mts.");
  return replaceAll(text,
                    "run scripts/extract-vcu-tables.ts against your own service-tool install (see README.md) and send "
                    "the diff.",
                    "add it as docs/service-mode.md's Parameter tables section describes and run "
                    "tools/generate-vcu-tables.mts.");
}

template <typename Write>
std::string build(Write write) {
  std::string text(64 * 1024, '\0');
  TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

std::string jsonString(std::string_view value) {
  return build([&](TextBuffer& out) { out.appendJsonString(value); });
}

void compare(const std::string& kind, const std::string& input, const std::string& mine, const std::string& theirs) {
  EMCOOL_CHECK(mine == theirs, "%s %s:\n  emcool    %s\n  reference %s", kind.c_str(), input.substr(0, 120).c_str(),
               mine.substr(0, 1500).c_str(), theirs.substr(0, 1500).c_str());
}

const char* extendedStatusName(ExtendedIsoTpResult::Status status) {
  switch (status) {
    case ExtendedIsoTpResult::Status::kIncomplete:
      return "incomplete";
    case ExtendedIsoTpResult::Status::kFlowControlRequired:
      return "flow-control-required";
    case ExtendedIsoTpResult::Status::kComplete:
      return "complete";
    case ExtendedIsoTpResult::Status::kIgnored:
      return "ignored";
    case ExtendedIsoTpResult::Status::kAbandoned:
      return "abandoned";
  }
  return "?";
}

std::string frameJson(const VcuFrame& frame) {
  return build([&](TextBuffer& out) {
    switch (frame.kind) {
      case VcuFrame::Kind::kPayload:
        out.append("{\"kind\":\"payload\",\"payload\":");
        out.appendJsonString(lowerHex(frame.payload));
        break;
      case VcuFrame::Kind::kMultiFrame:
        out.append("{\"kind\":\"multi-frame\",\"totalLength\":");
        out.appendInteger(frame.totalLength);
        break;
      case VcuFrame::Kind::kIgnored:
        out.append("{\"kind\":\"ignored\",\"reason\":");
        out.appendJsonString(frame.reason);
        break;
    }
    out.append("}");
  });
}

std::string replyJson(const VcuParameterReply& reply, int identifier) {
  return build([&](TextBuffer& out) {
    switch (reply.kind) {
      case VcuParameterReply::Kind::kRecord:
        out.append("{\"kind\":\"record\",\"identifier\":");
        out.appendInteger(identifier);
        out.append(",\"record\":");
        out.appendJsonString(lowerHex(reply.record));
        break;
      case VcuParameterReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"service\":");
        out.appendInteger(reply.service);
        out.append(",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        break;
      case VcuParameterReply::Kind::kIdentifierMismatch:
        out.append("{\"kind\":\"identifier-mismatch\",\"expected\":");
        out.appendInteger(reply.expected);
        out.append(",\"received\":");
        out.appendInteger(reply.received);
        break;
      case VcuParameterReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        break;
    }
    out.append("}");
  });
}

void writeOptionalNumber(TextBuffer& out, const std::optional<double>& value) {
  if (value) {
    out.appendNumber(*value);
  } else {
    out.append("null");
  }
}

std::optional<StorageType> storageTypeNamed(std::string_view name) {
  for (const StorageType type : {StorageType::kBool, StorageType::kByte, StorageType::kWord, StorageType::kDword}) {
    if (name == storageTypeName(type)) return type;
  }
  return std::nullopt;
}

void checkInterpret(const std::string& line) {
  const std::vector<std::string> words = fields(line, 3);
  const std::vector<std::uint8_t> record = parseHex(words[1]);
  // The encoding JSON, then the result's, then the mismatch text's: split on the "} " and "} \"" boundaries.
  const std::string rest = words[2];
  std::optional<RecordEncoding> encoding;
  std::size_t resultStart;
  if (rest.rfind("null ", 0) == 0) {
    resultStart = 5;
  } else {
    const std::size_t close = rest.find("} ");
    emcool::ble::JsonObjectReader reader;
    char scratch[256];
    const std::string encodingJson = rest.substr(0, close + 1);
    EMCOOL_CHECK(reader.read(encodingJson, scratch), "unreadable encoding %s", encodingJson.c_str());
    RecordEncoding parsed{*storageTypeNamed(reader.find("type")->text), std::nullopt, nullptr};
    const emcool::ble::JsonMember* isSigned = reader.find("signed");
    if (isSigned->type == emcool::ble::JsonType::kBool) parsed.isSigned = isSigned->text == "true";
    if (reader.find("describedBy") != nullptr) parsed.describedBy = "A8's firmware table";
    encoding = parsed;
    resultStart = close + 2;
  }
  const std::size_t resultEnd = rest.find("} ", resultStart);
  const std::string theirs = rest.substr(resultStart, resultEnd + 1 - resultStart);
  const std::string theirMismatch = rest.substr(resultEnd + 2);
  const VcuParameterValue value = interpretRecord(record, encoding);
  const std::string mine = build([&](TextBuffer& out) {
    out.append("{\"rawHex\":");
    out.appendJsonString(value.rawHex);
    out.append(",\"unsigned\":");
    out.appendNumber(value.unsignedValue);
    out.append(",\"signed\":");
    out.appendNumber(value.signedValue);
    out.append(",\"value\":");
    writeOptionalNumber(out, value.value);
    out.append(value.widthMismatch ? ",\"widthMismatch\":true}" : ",\"widthMismatch\":false}");
  });
  compare("interpretRecord", words[1], mine, theirs);
  const std::string mismatch =
      encoding && value.widthMismatch ? jsonString(describeWidthMismatch(record.size(), *encoding)) : "null";
  compare("describeWidthMismatch", words[1], mismatch, theirMismatch);
}

std::string flowControlJson(const std::optional<VcuFlowControl>& flow) {
  if (!flow) return "null";
  return build([&](TextBuffer& out) {
    switch (flow->status) {
      case VcuFlowControl::Status::kClearToSend:
        out.append("{\"status\":\"clear-to-send\",\"blockSize\":");
        out.appendInteger(flow->blockSize);
        out.append(",\"separationTimeMs\":");
        out.appendInteger(flow->separationTimeMs);
        out.append("}");
        break;
      case VcuFlowControl::Status::kWait:
        out.append("{\"status\":\"wait\"}");
        break;
      case VcuFlowControl::Status::kOverflow:
        out.append("{\"status\":\"overflow\"}");
        break;
      case VcuFlowControl::Status::kUnrecognised:
        out.append("{\"status\":\"unrecognised\",\"flowStatus\":");
        out.appendInteger(flow->flowStatus);
        out.append("}");
        break;
    }
  });
}

std::string multiFrameReplyJson(const VcuMultiFrameReply& reply) {
  return build([&](TextBuffer& out) {
    switch (reply.kind) {
      case VcuMultiFrameReply::Kind::kPositive:
        out.append("{\"kind\":\"positive\",\"service\":");
        out.appendInteger(reply.service);
        out.append(",\"body\":");
        out.appendJsonString(lowerHex(reply.body));
        break;
      case VcuMultiFrameReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"service\":");
        out.appendInteger(reply.service);
        out.append(",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        break;
      case VcuMultiFrameReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        break;
    }
    out.append("}");
  });
}

void checkGate(const std::string& line) {
  const std::vector<std::string> words = fields(line, 3);
  GateReadings readings;
  std::string flat = words[1];
  std::size_t at = 0;
  std::vector<std::string> keys;
  while (at < flat.size()) {
    std::size_t end = flat.find(';', at);
    if (end == std::string::npos) end = flat.size();
    const std::string item = flat.substr(at, end - at);
    keys.push_back(item.substr(0, item.find('=')));
    at = end + 1;
  }
  at = 0;
  std::size_t keyIndex = 0;
  const std::span<const std::string_view> gateKeys = serviceGateSignalKeys();
  while (at < flat.size()) {
    std::size_t end = flat.find(';', at);
    if (end == std::string::npos) end = flat.size();
    const std::string item = flat.substr(at, end - at);
    const std::size_t equals = item.find('=');
    const std::size_t atSign = item.find('@');
    const std::string value = item.substr(equals + 1, atSign - equals - 1);
    const std::string age = item.substr(atSign + 1);
    GateSample sample;
    if (value != "null") sample.value = std::strtod(value.c_str(), nullptr);
    if (age != "null") sample.ageMs = std::strtod(age.c_str(), nullptr);
    EMCOOL_CHECK(keyIndex < gateKeys.size() && gateKeys[keyIndex] == keys[keyIndex], "gate key %zu is %s", keyIndex,
                 keys[keyIndex].c_str());
    readings.samples.emplace_back(gateKeys[keyIndex], sample);
    keyIndex++;
    at = end + 1;
  }
  const ServiceGateVerdict verdict = evaluateServiceGate(readings);
  compare("evaluateServiceGate", words[1], build([&](TextBuffer& out) { writeVerdictJson(out, verdict); }), words[2]);
}

std::string rowJson(const VcuParameterRow& row) {
  return build([&](TextBuffer& out) { writeRowJson(out, row); });
}

VcuParameterRow parseRow(const std::string& json) {
  const std::optional<VcuParameterRow> row = parseRowJson(json);
  EMCOOL_CHECK(row.has_value(), "unparseable row %s", json.substr(0, 200).c_str());
  if (!row) std::exit(1);
  // Reading a row back must give it back.
  compare("row round trip", json.substr(0, 60), rowJson(*row), json);
  return *row;
}

VcuReadOutcome parseOutcome(const std::string& json) {
  emcool::ble::JsonObjectReader reader;
  std::vector<char> scratch(json.size() + 16);
  EMCOOL_CHECK(reader.read(json, scratch), "unreadable outcome %s", json.c_str());
  VcuReadOutcome outcome;
  outcome.micro = *microNamed(reader.find("micro")->text);
  outcome.index = static_cast<int>(reader.find("index")->number);
  outcome.identifier = static_cast<int>(reader.find("identifier")->number);
  outcome.status = *readStatusNamed(reader.find("status")->text);
  if (const auto* record = reader.find("record")) outcome.record = parseHex(record->text);
  if (const auto* code = reader.find("negativeResponseCode")) outcome.negativeResponseCode = static_cast<int>(code->number);
  if (const auto* description = reader.find("description")) outcome.description = std::string(description->text);
  if (const auto* reason = reader.find("reason")) outcome.reason = std::string(reason->text);
  return outcome;
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_vcu <golden file>\n");
    return 2;
  }
  std::ifstream input(arguments[1]);
  std::map<std::string, int> checked;
  ExtendedIsoTpReassembler reassembler;
  ParameterNames names;
  std::vector<VcuParameterRow> snapshotRows;
  std::vector<VcuParameterRow> retabledRows;
  std::vector<VcuParameterRow> previousRows;
  std::vector<std::string> diffs;
  int tableType = kDefaultTableType;
  std::string line;
  while (std::getline(input, line)) {
    const std::string kind = line.substr(0, line.find(' '));
    checked[kind]++;
    if (kind == "XI") {
      reassembler = ExtendedIsoTpReassembler(std::stoi(fields(line, 2)[1]));
    } else if (kind == "XP") {
      const std::vector<std::string> words = fields(line, 6);
      const ExtendedIsoTpResult result = reassembler.push(parseHex(words[1]));
      const std::string mine = std::string(extendedStatusName(result.status)) + " " + std::to_string(result.totalLength) +
                               " " + (result.status == ExtendedIsoTpResult::Status::kComplete ? lowerHex(result.payload) : "-") +
                               " " + (result.reason.empty() ? "-" : result.reason);
      compare("extended ISO-TP", words[1], mine, words[2] + " " + words[3] + " " + words[4] + " " + words[5]);
    } else if (kind == "PF") {
      const std::vector<std::string> words = fields(line, 3);
      compare("parseResponseFrame", words[1], frameJson(parseResponseFrame(parseHex(words[1]))), words[2]);
    } else if (kind == "PR") {
      const std::vector<std::string> words = fields(line, 4);
      const int identifier = std::stoi(words[2]);
      compare("decodeParameterReply", words[1], replyJson(decodeParameterReply(parseHex(words[1]), identifier), identifier),
              words[3]);
    } else if (kind == "IR") {
      checkInterpret(line);
    } else if (kind == "SEG") {
      const std::vector<std::string> words = fields(line, 4);
      std::string mine;
      for (const auto& frame : segmentRequestPayload(*microNamed(words[1]), parseHex(words[2]))) {
        if (!mine.empty()) mine += ",";
        mine += lowerHex(frame);
      }
      compare("segmentRequestPayload", words[2], mine, words[3]);
    } else if (kind == "FC") {
      const std::vector<std::string> words = fields(line, 3);
      compare("parseFlowControlFrame", words[1], flowControlJson(parseFlowControlFrame(parseHex(words[1]))), words[2]);
    } else if (kind == "MR") {
      const std::vector<std::string> words = fields(line, 4);
      compare("decodeMultiFrameReply", words[1],
              multiFrameReplyJson(decodeMultiFrameReply(parseHex(words[1]), std::stoi(words[2]))), words[3]);
    } else if (kind == "SG") {
      checkGate(line);
    } else if (kind == "DT") {
      const std::vector<std::string> words = fields(line, 3);
      const double raw = std::stod(words[1]);
      std::string twins = "[";
      for (const int twin : contentTwinsOf(static_cast<int>(raw))) twins += (twins.size() > 1 ? "," : "") + std::to_string(twin);
      twins += "]";
      compare("describeTableType", words[1], jsonString(describeTableType(raw)) + " " + twins, words[2]);
    } else if (kind == "OUT") {
      const std::vector<std::string> words = fields(line, 3);
      tableType = std::stoi(words[1]);
      names.select(tableType);
      const VcuReadOutcome outcome = parseOutcome(words[2]);
      std::getline(input, line);
      compare("toParameterRow", words[2], rowJson(toParameterRow(outcome, names)), line.substr(4));
      checked["TPR"]++;
    } else if (kind == "ROW") {
      const std::size_t split = line.rfind("} \"");
      const VcuParameterRow row = parseRow(line.substr(4, split + 1 - 4));
      compare("describeRow", line.substr(4, 60), jsonString(describeRow(row)), line.substr(split + 2));
      snapshotRows.push_back(row);
    } else if (kind == "REP") {
      const std::vector<std::string> words = fields(line, 3);
      names.select(std::stoi(words[1]));
      std::sort(snapshotRows.begin(), snapshotRows.end(),
                [](const VcuParameterRow& left, const VcuParameterRow& right) { return left.index < right.index; });
      const auto rowAt = [&](int index) -> const VcuParameterRow* {
        for (const VcuParameterRow& row : snapshotRows) {
          if (row.index == index) return &row;
        }
        return nullptr;
      };
      const TableTypeReport report = reportTableType(rowAt, names);
      compare("reportTableType", words[1], build([&](TextBuffer& out) { writeReportJson(out, report); }),
              emcoolWording(words[2]));
      const Retabler retabler(report, names);
      retabledRows.clear();
      for (const VcuParameterRow& row : snapshotRows) retabledRows.push_back(retabler.retable(row));
    } else if (kind == "RET") {
      const std::size_t position = static_cast<std::size_t>(checked["RET"] - 1) - static_cast<std::size_t>(checked["RETDONE"]);
      EMCOOL_CHECK(position < retabledRows.size(), "more retabled rows than rows");
      if (position < retabledRows.size()) compare("retableSnapshot", line.substr(4, 60), rowJson(retabledRows[position]), line.substr(4));
    } else if (kind == "RETEND") {
      checked["RETDONE"] += static_cast<int>(retabledRows.size());
      snapshotRows.clear();
      previousRows.clear();
      diffs.clear();
    } else if (kind == "DPREV") {
      previousRows.push_back(parseRow(line.substr(6)));
    } else if (kind == "DIFF") {
      diffs.push_back(line.substr(5));
    } else if (kind == "DIFFEND") {
      std::size_t before = 0;
      std::size_t after = 0;
      std::vector<std::string> mine;
      diffSnapshots(
          [&]() -> std::optional<VcuParameterRow> {
            if (before >= previousRows.size()) return std::nullopt;
            return previousRows[before++];
          },
          [&]() -> std::optional<VcuParameterRow> {
            if (after >= retabledRows.size()) return std::nullopt;
            return retabledRows[after++];
          },
          [&](const VcuParameterChange& change) { mine.push_back(jsonString(describeChange(change))); });
      std::string mineText, theirText;
      for (const std::string& change : mine) mineText += change + "\n";
      for (const std::string& change : diffs) theirText += change + "\n";
      compare("diffSnapshots", "", mineText, theirText);
      previousRows.clear();
      diffs.clear();
    } else if (kind == "USE") {
      names.select(std::stoi(fields(line, 2)[1]));
    } else if (kind == "SEL") {
      const std::vector<std::string> words = fields(line, 3);
      const ParameterNames::Selection selection = names.select(std::stoi(words[1]));
      const std::string mine = build([&](TextBuffer& out) {
        out.append(selection.ok ? "{\"ok\":true,\"tableType\":" : "{\"ok\":false,\"tableType\":");
        out.appendInteger(selection.tableType);
        out.append(",\"message\":");
        out.appendJsonString(selection.message);
        out.append("}");
      });
      compare("selectParameterTable", words[1], mine, emcoolWording(words[2]));
    } else if (kind == "FW") {
      const std::vector<std::string> words = fields(line, 3);
      const A8FirmwareRow* row = firmwareRowFor(Micro::kA8, kCalibrationBank, std::stoi(words[1]));
      EMCOOL_CHECK(row != nullptr, "no firmware row %s", words[1].c_str());
      if (row != nullptr) compare("describeFirmwareRow", words[1], jsonString(describeFirmwareRow(*row)), words[2]);
    } else {
      std::fprintf(stderr, "check_vcu: unknown line %s\n", line.substr(0, 60).c_str());
      return 2;
    }
  }
  std::string summary;
  for (const auto& [kind, total] : checked) summary += " " + kind + "=" + std::to_string(total);
  std::printf("check_vcu:%s; %d failures\n", summary.c_str(), emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
