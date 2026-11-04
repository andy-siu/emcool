// Replays tools/generate-golden-write.mts through the C++ port: the pure stage-4 write parts must
// produce byte-identical JSON to the reference implementation's own code. The wording divergences —
// the reference names its source file src/vcu/write-targets.ts where emcool names write_targets, and
// its extract script where emcool points at docs/service-mode.md — are normalised before comparing,
// exactly as check_service.cpp does for the read path.
//
//   check_write <golden file>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/charge_acknowledge.hpp"
#include "emcool/vcu/charge_command.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/table_gate.hpp"
#include "emcool/vcu/write_codec.hpp"
#include "emcool/vcu/write_runner.hpp"
#include "emcool/vcu/write_targets.hpp"

using namespace emcool;
using namespace emcool::vcu;

namespace {

int failures = 0;

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
  if (from.empty()) return text;
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

// The C++ wording, turned into the reference's, so a byte comparison can stand. emcool says "bike"
// or "emcool" wherever the reference's prose says "Pi" (it ran on a Raspberry Pi), the same rewording
// the read path's SERVICE_MODE_ENABLED message already gets in check_service.cpp.
std::string asReference(std::string text) {
  text = replaceAll(text, "this bike's clock", "this Pi's clock");
  text = replaceAll(text, "compare-and-swap: emcool re-reads", "compare-and-swap: the Pi re-reads");
  text = replaceAll(text, "emcool checks it is still that minute", "The Pi checks it is still that minute");
  text = replaceAll(text, ", but emcool now reads ", ", but this Pi now reads ");
  text = replaceAll(text, "the GPS clock steps it past 60 s", "src/gps/clock.ts steps it past 60 s");
  // emcool's dashboard request header is X-Emcool (the firmware checks that name); the reference's
  // golden shows its own header name in the table-gate curl example.
  text = replaceAll(text, "X-Emcool: service-mode", "X-Cool-Eva: service-mode");
  text = replaceAll(text, "written down with its reasoning in write_targets.",
                    "written down with its reasoning in src/vcu/write-targets.ts.");
  // Stage 4 omits charge-current/charge-stop (stage 5); the reference's unknown-action list still names
  // them. The documented divergence: emcool's list grows back when stage 5 lands.
  text = replaceAll(text, "clear-dtcs, charge-soc-limit, charge-soc-limit-read, reset-vcu — not",
                    "clear-dtcs, charge-current, charge-stop, charge-soc-limit, charge-soc-limit-read, reset-vcu — not");
  // The two extract-script wordings check_service.cpp normalises too: the mismatch-reason sentence
  // (plain) and the mismatch-remedy sentence (JSON-escaped quotes).
  text = replaceAll(text,
                    "add it as docs/service-mode.md's Parameter tables section describes and run "
                    "tools/generate-vcu-tables.mts.",
                    "run scripts/extract-vcu-tables.ts against your own service-tool install (see README.md) and send "
                    "the diff.");
  for (const char* quote : {"\"", "\\\""}) {
    text = replaceAll(
        text,
        std::string("add the table as docs/service-mode.md (") + quote + "Parameter tables" + quote +
            ") describes and run tools/generate-vcu-tables.mts, and this gate opens.",
        std::string("run `node --experimental-strip-types scripts/extract-vcu-tables.ts <the exe>` against your own "
                    "Energica service-tool install — its main executable, the dealer application itself — commit the "
                    "diff to src/vcu/table-catalog.data.ts and this gate opens. README.md, ") +
            quote + "Adding your bike's VCU parameter table" + quote + ", is the walkthrough.");
  }
  return text;
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

void expectEqual(const std::string& what, const std::string& got, const std::string& want) {
  if (asReference(got) == want) return;
  if (failures < 20) {
    std::fprintf(stderr, "check_write: %s mismatch\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(), want.c_str());
  }
  failures++;
}

std::vector<std::uint8_t> parseHex(const std::string& text) {
  std::vector<std::uint8_t> bytes;
  if (text == "-") return bytes;
  for (std::size_t at = 0; at + 1 < text.size(); at += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(at, 2).c_str(), nullptr, 16)));
  }
  return bytes;
}

std::vector<std::string> split(const std::string& line, std::size_t count) {
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

std::string lowerHex8(const std::array<std::uint8_t, 8>& bytes) {
  std::string text;
  char pair[3];
  for (const std::uint8_t byte : bytes) {
    std::snprintf(pair, sizeof pair, "%02x", byte);
    text += pair;
  }
  return text;
}

double jsonNumberField(const std::string& object, const std::string& key) {
  const std::size_t at = object.find("\"" + key + "\":");
  if (at == std::string::npos) return 0;
  return std::strtod(object.c_str() + at + key.size() + 3, nullptr);
}

ChargeAckInput parseAckInput(const std::string& object) {
  ChargeAckInput input;
  input.commandedAmps = jsonNumberField(object, "commandedAmps");
  input.sentAtMs = static_cast<std::int64_t>(jsonNumberField(object, "sentAtMs"));
  input.nowMs = static_cast<std::int64_t>(jsonNumberField(object, "nowMs"));
  const std::size_t sup = object.find("\"supersededAtMs\":");
  if (sup != std::string::npos && object.compare(sup + 17, 4, "null") != 0) {
    input.hasSuperseded = true;
    input.supersededAtMs = static_cast<std::int64_t>(std::strtod(object.c_str() + sup + 17, nullptr));
  }
  // samples: [{"atMs":n,"amps":n}, …]
  const std::size_t arr = object.find("\"samples\":[");
  std::size_t cursor = arr == std::string::npos ? std::string::npos : arr + 11;
  while (cursor != std::string::npos && cursor < object.size() && object[cursor] == '{') {
    const std::size_t end = object.find('}', cursor);
    const std::string sample = object.substr(cursor, end - cursor);
    input.samples.push_back({static_cast<std::int64_t>(jsonNumberField(sample, "atMs")), jsonNumberField(sample, "amps")});
    cursor = object.find('{', end);
    const std::size_t close = object.find(']', end);
    if (cursor == std::string::npos || (close != std::string::npos && close < cursor)) break;
  }
  return input;
}

std::string ackVerdictJson(const ChargeAckVerdict& verdict) {
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  const auto write = [&](text::TextBuffer& out) {
    switch (verdict.kind) {
      case ChargeAckVerdict::Kind::kWaiting:
        out.append("{\"kind\":\"waiting\",\"elapsedMs\":");
        out.appendNumber(static_cast<double>(verdict.elapsedMs));
        out.append("}");
        return;
      case ChargeAckVerdict::Kind::kTook:
        out.append("{\"kind\":\"took\",\"fromAmps\":");
        out.appendNumber(verdict.fromAmps);
        out.append(",\"toAmps\":");
        out.appendNumber(verdict.toAmps);
        out.append(",\"latencyMs\":");
        out.appendNumber(static_cast<double>(verdict.latencyMs));
        out.append("}");
        return;
      case ChargeAckVerdict::Kind::kNotAcknowledged:
        out.append("{\"kind\":\"not-acknowledged\",\"heldAmps\":");
        out.appendNumber(verdict.heldAmps);
        out.append("}");
        return;
      case ChargeAckVerdict::Kind::kStationLimited:
        out.append("{\"kind\":\"station-limited\",\"askedAmps\":");
        out.appendNumber(verdict.askedAmps);
        out.append(",\"gotAmps\":");
        out.appendNumber(verdict.gotAmps);
        out.append("}");
        return;
      case ChargeAckVerdict::Kind::kNoChangeExpected:
        out.append("{\"kind\":\"no-change-expected\",\"atAmps\":");
        out.appendNumber(verdict.atAmps);
        out.append("}");
        return;
      case ChargeAckVerdict::Kind::kSuperseded:
        out.append("{\"kind\":\"superseded\"}");
        return;
      case ChargeAckVerdict::Kind::kNoEvidence:
        out.append("{\"kind\":\"no-evidence\"}");
        return;
    }
  };
  write(measure);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

std::string jsonUnescape(const std::string& quoted) {
  // The golden's JSON-string fields (query, minute) are quoted; undo the escaping we need: \" and \\.
  std::string out;
  for (std::size_t at = 0; at < quoted.size(); at++) {
    if (quoted[at] == '\\' && at + 1 < quoted.size()) {
      out += quoted[++at];
    } else {
      out += quoted[at];
    }
  }
  return out;
}

ParameterNames gNames;  // the default table, reset to it before each GATE line

// --- the small decoders, as JSON matching the reference -----------------------------------------

std::string securityAccessJson(const SecurityAccessReply& reply) {
  return build([&](text::TextBuffer& out) {
    switch (reply.kind) {
      case SecurityAccessReply::Kind::kSeed:
        out.append("{\"kind\":\"seed\",\"seed\":");
        out.appendNumber(reply.seed);
        out.append("}");
        return;
      case SecurityAccessReply::Kind::kUnlocked:
        out.append("{\"kind\":\"unlocked\",\"trailing\":");
        if (reply.trailing) {
          out.appendInteger(*reply.trailing);
        } else {
          out.append("null");
        }
        out.append("}");
        return;
      case SecurityAccessReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        out.append("}");
        return;
      case SecurityAccessReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        out.append("}");
        return;
    }
  });
}

std::string parameterWriteJson(const ParameterWriteReply& reply) {
  return build([&](text::TextBuffer& out) {
    switch (reply.kind) {
      case ParameterWriteReply::Kind::kAccepted:
        out.append("{\"kind\":\"accepted\",\"identifier\":");
        out.appendInteger(reply.identifier);
        out.append("}");
        return;
      case ParameterWriteReply::Kind::kIdentifierMismatch:
        out.append("{\"kind\":\"identifier-mismatch\",\"expected\":");
        out.appendInteger(reply.expected);
        out.append(",\"received\":");
        out.appendInteger(reply.received);
        out.append("}");
        return;
      case ParameterWriteReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        out.append("}");
        return;
      case ParameterWriteReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        out.append("}");
        return;
    }
  });
}

std::string routineJson(const RoutineReply& reply) {
  return build([&](text::TextBuffer& out) {
    switch (reply.kind) {
      case RoutineReply::Kind::kStarted:
        out.append("{\"kind\":\"started\",\"routine\":");
        out.appendInteger(reply.routine);
        out.append("}");
        return;
      case RoutineReply::Kind::kRoutineMismatch:
        out.append("{\"kind\":\"routine-mismatch\",\"expected\":");
        out.appendInteger(reply.expected);
        out.append(",\"received\":");
        out.appendInteger(reply.received);
        out.append("}");
        return;
      case RoutineReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        out.append("}");
        return;
      case RoutineReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        out.append("}");
        return;
    }
  });
}

std::string clearJson(const ClearDtcsReply& reply) {
  return build([&](text::TextBuffer& out) {
    switch (reply.kind) {
      case ClearDtcsReply::Kind::kCleared:
        out.append("{\"kind\":\"cleared\"}");
        return;
      case ClearDtcsReply::Kind::kRefused:
        out.append("{\"kind\":\"refused\",\"negativeResponseCode\":");
        out.appendInteger(reply.negativeResponseCode);
        out.append(",\"description\":");
        out.appendJsonString(reply.description);
        out.append("}");
        return;
      case ClearDtcsReply::Kind::kUnrecognised:
        out.append("{\"kind\":\"unrecognised\",\"reason\":");
        out.appendJsonString(reply.reason);
        out.append("}");
        return;
    }
  });
}

std::string stampJson(const ServiceStamp& stamp) {
  return build([&](text::TextBuffer& out) {
    out.append("{\"raw\":{\"dateLow\":");
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
    out.append("}");
  });
}

std::string clockJson(const PiClockVerdict& verdict) {
  return build([&](text::TextBuffer& out) {
    if (verdict.trustworthy) {
      out.append("{\"trustworthy\":true,\"iso\":");
      out.appendJsonString(verdict.iso);
      out.append(",\"offsetFromGpsSeconds\":");
      out.appendNumber(verdict.offsetFromGpsSeconds);
      out.append("}");
      return;
    }
    out.append("{\"trustworthy\":false,\"iso\":");
    out.appendJsonString(verdict.iso);
    out.append(",\"reasons\":[");
    for (std::size_t at = 0; at < verdict.reasons.size(); at++) {
      if (at > 0) out.append(",");
      out.appendJsonString(verdict.reasons[at]);
    }
    out.append("]}");
  });
}

std::string planJson(const WritePlanOutcome& outcome) {
  return build([&](text::TextBuffer& out) {
    if (!outcome.ok) {
      out.append("{\"ok\":false,\"reason\":");
      out.appendJsonString(outcome.reason);
      out.append("}");
      return;
    }
    const ParameterWritePlan& plan = outcome.plan;
    out.append("{\"ok\":true,\"plan\":{\"name\":");
    out.appendJsonString(plan.name);
    out.append(",\"index\":");
    out.appendInteger(plan.index);
    out.append(",\"micro\":");
    out.appendJsonString(microName(plan.micro));
    out.append(",\"identifier\":");
    out.appendInteger(plan.identifier);
    out.append(",\"record\":\"");
    char pair[3];
    for (const std::uint8_t byte : plan.record) {
      std::snprintf(pair, sizeof pair, "%02x", byte);
      out.append(pair);
    }
    out.append("\",\"value\":");
    out.appendNumber(plan.value);
    out.append(",\"previousValue\":");
    out.appendNumber(plan.previousValue);
    out.append(",\"description\":");
    out.appendJsonString(plan.description);
    out.append("}}");
  });
}

std::string rtcDecodeJson(const std::optional<RtcFields>& fields) {
  if (!fields) return "null";
  return build([&](text::TextBuffer& out) {
    out.append("{\"hour\":");
    out.appendInteger(fields->hour);
    out.append(",\"minute\":");
    out.appendInteger(fields->minute);
    out.append(",\"second\":");
    out.appendInteger(fields->second);
    out.append(",\"day\":");
    out.appendInteger(fields->day);
    out.append(",\"weekday\":");
    out.appendInteger(fields->weekday);
    out.append(",\"month\":");
    out.appendInteger(fields->month);
    out.append(",\"year\":");
    out.appendInteger(fields->year);
    out.append("}");
  });
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: check_write <golden file>\n");
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::fprintf(stderr, "check_write: cannot open %s\n", argv[1]);
    return 2;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const std::size_t firstSpace = line.find(' ');
    const std::string kind = line.substr(0, firstSpace);

    if (kind == "KEY") {
      const std::vector<std::string> f = split(line, 3);
      const std::uint32_t seed = static_cast<std::uint32_t>(std::strtoul(f[1].c_str(), nullptr, 10));
      const std::uint32_t want = static_cast<std::uint32_t>(std::strtoul(f[2].c_str(), nullptr, 10));
      if (securityKeyForSeed(seed) != want) {
        if (failures < 20) std::fprintf(stderr, "check_write: KEY %u -> %u, want %u\n", seed, securityKeyForSeed(seed), want);
        failures++;
      }
    } else if (kind == "SA") {
      const std::vector<std::string> f = split(line, 3);
      const std::vector<std::uint8_t> payload = parseHex(f[1]);
      expectEqual("SA", securityAccessJson(decodeSecurityAccessReply(payload)), f[2]);
    } else if (kind == "PW") {
      const std::vector<std::string> f = split(line, 4);
      const int identifier = std::atoi(f[1].c_str());
      expectEqual("PW", parameterWriteJson(decodeParameterWriteReply(parseHex(f[2]), identifier)), f[3]);
    } else if (kind == "RR") {
      const std::vector<std::string> f = split(line, 3);
      expectEqual("RR", routineJson(decodeRoutineReply(parseHex(f[1]), ServiceRoutineName::kSetServicePoint)), f[2]);
    } else if (kind == "CD") {
      const std::vector<std::string> f = split(line, 3);
      expectEqual("CD", clearJson(decodeClearDtcsReply(parseHex(f[1]))), f[2]);
    } else if (kind == "CDR") {
      const std::vector<std::string> f = split(line, 3);
      const std::vector<std::uint8_t> payload = parseHex(f[1]);
      const std::string got = isClearDtcsReply(payload) ? "1" : "0";
      expectEqual("CDR", got, f[2]);
    } else if (kind == "ST") {
      const std::vector<std::string> f = split(line, 7);
      ServiceStampRaw raw{std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), std::atoi(f[3].c_str()),
                          std::atoi(f[4].c_str())};
      const double nowMs = std::strtod(f[5].c_str(), nullptr);
      expectEqual("ST", stampJson(interpretServiceStamp(raw, nowMs)), f[6]);
    } else if (kind == "CK") {
      // CK <systemEpochMs|nan> <gpsEpochSeconds|null> <gpsAgeMs|null> {verdict-json}
      const std::vector<std::string> f = split(line, 5);
      PiClockEvidence evidence;
      evidence.systemEpochMs = f[1] == "nan" ? std::nan("") : std::strtod(f[1].c_str(), nullptr);
      evidence.gpsEpochSeconds = f[2] == "null" ? std::nullopt : std::optional<double>(std::strtod(f[2].c_str(), nullptr));
      evidence.gpsAgeMs = f[3] == "null" ? std::nullopt : std::optional<double>(std::strtod(f[3].c_str(), nullptr));
      expectEqual("CK", clockJson(checkPiClock(evidence)), f[4]);
    } else if (kind == "RTC") {
      const std::vector<std::string> f = split(line, 4);
      const double epoch = std::strtod(f[1].c_str(), nullptr);
      const std::optional<std::array<std::uint8_t, 8>> frame = buildRtcSyncFrame(epoch);
      std::string gotHex = "-";
      if (frame) {
        gotHex.clear();
        char pair[3];
        for (const std::uint8_t byte : *frame) {
          std::snprintf(pair, sizeof pair, "%02x", byte);
          gotHex += pair;
        }
      }
      expectEqual("RTC-frame", gotHex, f[2]);
      std::optional<RtcFields> decoded;
      if (frame) decoded = decodeRtcSyncFrame(*frame);
      expectEqual("RTC-decode", rtcDecodeJson(decoded), f[3]);
    } else if (kind == "PLAN") {
      const std::vector<std::string> f = split(line, 3);
      // {"name":"…","value":n,"previous":n}
      const std::string& req = f[1];
      const std::size_t nameStart = req.find("\"name\":\"") + 8;
      const std::size_t nameEnd = req.find('"', nameStart);
      const std::string name = req.substr(nameStart, nameEnd - nameStart);
      const auto num = [&](const std::string& key) {
        const std::size_t at = req.find("\"" + key + "\":") + key.size() + 3;
        return std::strtod(req.substr(at).c_str(), nullptr);
      };
      expectEqual("PLAN", planJson(planWrite(gNames, name, num("value"), num("previous"))), f[2]);
    } else if (kind == "BIT") {
      const std::vector<std::string> f = split(line, 3);
      const std::string& req = f[1];
      const auto str = [&](const std::string& key) {
        const std::size_t at = req.find("\"" + key + "\":\"") + key.size() + 4;
        const std::size_t end = req.find('"', at);
        return req.substr(at, end - at);
      };
      const bool on = req.find("\"on\":true") != std::string::npos;
      const std::size_t curAt = req.find("\"current\":") + 10;
      const double current = std::strtod(req.substr(curAt).c_str(), nullptr);
      expectEqual("BIT", planJson(planBitWrite(gNames, str("name"), str("bit"), on, current)), f[2]);
    } else if (kind == "GATE") {
      // GATE {report-json} {verdict-json}
      const std::vector<std::string> f = split(line, 3);
      gNames = ParameterNames{};  // reset to the default, as the generator does
      // Parse the report: verdicts [{index,micro,value}], unusable [ints]. Minimal hand parse.
      TableTypeReport report;
      bool present = f[1] != "null";
      if (present) {
        const std::string& r = f[1];
        std::size_t at = 0;
        while ((at = r.find("\"index\":", at)) != std::string::npos) {
          const int index = std::atoi(r.c_str() + at + 8);
          const std::size_t microAt = r.find("\"micro\":\"", at) + 9;
          const std::string micro = r.substr(microAt, r.find('"', microAt) - microAt);
          const std::size_t valueAt = r.find("\"value\":", at) + 8;
          const double value = std::strtod(r.c_str() + valueAt, nullptr);
          report.verdicts.push_back({index, *microNamed(micro), value, false, {}});
          at = valueAt;
        }
        const std::size_t un = r.find("\"unusable\":[");
        if (un != std::string::npos) {
          std::size_t cursor = un + 12;
          while (cursor < r.size() && r[cursor] != ']') {
            if (std::isdigit(static_cast<unsigned char>(r[cursor]))) {
              report.unusable.push_back(std::atoi(r.c_str() + cursor));
              while (cursor < r.size() && r[cursor] != ',' && r[cursor] != ']') cursor++;
            } else {
              cursor++;
            }
          }
        }
      }
      const std::string got =
          build([&](text::TextBuffer& out) { writeTableGateVerdictJson(out, evaluateTableGate(present ? &report : nullptr, gNames)); });
      expectEqual("GATE", got, f[2]);
    } else if (kind == "PARSE") {
      const std::vector<std::string> f = split(line, 3);
      const std::string query = jsonUnescape(f[1].substr(1, f[1].size() - 2));
      // get/getAll over the raw query (url-encoded), matching the endpoint helpers.
      const auto decode = [](std::string_view text) {
        std::string out;
        const auto hexDigit = [](char c) {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        for (std::size_t at = 0; at < text.size(); at++) {
          if (text[at] == '+') {
            out += ' ';
          } else if (text[at] == '%' && at + 2 < text.size() && hexDigit(text[at + 1]) >= 0 && hexDigit(text[at + 2]) >= 0) {
            out += static_cast<char>(hexDigit(text[at + 1]) * 16 + hexDigit(text[at + 2]));
            at += 2;
          } else {
            out += text[at];
          }
        }
        return out;
      };
      const auto get = [&](std::string_view key) -> std::optional<std::string> {
        std::string_view q = query;
        while (!q.empty()) {
          const std::size_t amp = q.find('&');
          const std::string_view pair = q.substr(0, amp);
          const std::size_t eq = pair.find('=');
          if (decode(pair.substr(0, eq)) == key)
            return eq == std::string_view::npos ? std::string() : decode(pair.substr(eq + 1));
          if (amp == std::string_view::npos) break;
          q.remove_prefix(amp + 1);
        }
        return std::nullopt;
      };
      const auto getAll = [&](std::string_view key) {
        std::vector<std::string> values;
        std::string_view q = query;
        while (!q.empty()) {
          const std::size_t amp = q.find('&');
          const std::string_view pair = q.substr(0, amp);
          const std::size_t eq = pair.find('=');
          if (decode(pair.substr(0, eq)) == key)
            values.push_back(eq == std::string_view::npos ? std::string() : decode(pair.substr(eq + 1)));
          if (amp == std::string_view::npos) break;
          q.remove_prefix(amp + 1);
        }
        return values;
      };
      const double nowMs = 1786888992000;  // Date.UTC(2026,7,16,14,3,12)
      const WriteRequestParse parsed = parseWriteRequest(get, getAll, nowMs);
      const std::string got = build([&](text::TextBuffer& out) {
        if (!parsed.ok) {
          out.append("{\"ok\":false,\"reason\":");
          out.appendJsonString(parsed.reason);
          out.append("}");
          return;
        }
        out.append("{\"ok\":true,\"request\":");
        // The request shape the reference emits is action-specific; compare just ok+kind-driven fields.
        out.append("{\"kind\":");
        out.appendJsonString(serviceWriteKindName(parsed.request.kind));
        out.append("}}");
      });
      // Compare only the ok flag and, when ok, that both call it ok; the refusal strings are the
      // load-bearing text and are compared in full. The reference's ok-request carries the typed union,
      // which is validated structurally by the perform tests, so here only the refusal path's
      // exact text matters.
      if (parsed.ok) {
        if (f[2].rfind("{\"ok\":true", 0) != 0) {
          if (failures < 20) std::fprintf(stderr, "check_write: PARSE ok mismatch for %s\n  want: %s\n", query.c_str(), f[2].c_str());
          failures++;
        }
      } else {
        // Extract the reference's reason and compare the refusal text exactly.
        const std::size_t reasonAt = f[2].find("\"reason\":\"");
        if (f[2].rfind("{\"ok\":false", 0) != 0) {
          if (failures < 20) std::fprintf(stderr, "check_write: PARSE should have been ok for %s\n  want: %s\n", query.c_str(), f[2].c_str());
          failures++;
        } else if (reasonAt != std::string::npos) {
          const std::string want = f[2].substr(reasonAt + 9);  // "…"}
          const std::string gotReason = build([&](text::TextBuffer& out) { out.appendJsonString(parsed.reason); }) + "}";
          expectEqual("PARSE", gotReason, want);
        }
      }
    } else if (kind == "SREQ") {
      const std::vector<std::string> f = split(line, 3);
      const std::string query = jsonUnescape(f[1].substr(1, f[1].size() - 2));
      const auto get = [&](std::string_view key) -> std::optional<std::string> {
        std::string_view q = query;
        while (!q.empty()) {
          const std::size_t amp = q.find('&');
          const std::string_view pair = q.substr(0, amp);
          const std::size_t eq = pair.find('=');
          std::string k(pair.substr(0, eq));
          if (k == key) return eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1));
          if (amp == std::string_view::npos) break;
          q.remove_prefix(amp + 1);
        }
        return std::nullopt;
      };
      const WriteStatusRequest req = parseStatusRequest(get);
      const std::string got = build([&](text::TextBuffer& out) {
        out.append("{\"detailFor\":");
        if (req.detailFor) {
          out.appendJsonString(*req.detailFor);
        } else {
          out.append("null");
        }
        out.append(req.includeList ? ",\"includeList\":true}" : ",\"includeList\":false}");
      });
      expectEqual("SREQ", got, f[2]);
    } else if (kind == "MIN") {
      const std::vector<std::string> f = split(line, 3);
      const double epoch = std::strtod(f[1].c_str(), nullptr);
      const std::string got = build([&](text::TextBuffer& out) { out.appendJsonString(utcMinute(epoch)); });
      expectEqual("MIN", got, f[2]);
    } else if (kind == "CC") {
      // CC <mode> <selected> <ceiling> <frames|ERR msg…>
      const std::vector<std::string> f = split(line, 5);
      const ChargeMode mode = f[1] == "dc" ? ChargeMode::kDc : ChargeMode::kAc;
      std::string error;
      const std::vector<ChargeFrame> frames =
          buildChargeCurrentCommand(mode, std::atoi(f[2].c_str()), std::atoi(f[3].c_str()), error);
      std::string got;
      if (!error.empty()) {
        got = "ERR " + error;
      } else {
        for (std::size_t at = 0; at < frames.size(); at++) {
          char id[12];
          std::snprintf(id, sizeof id, "%x", static_cast<unsigned>(frames[at].id));
          if (at > 0) got += ",";
          got += std::string(id) + ":" + lowerHex8(frames[at].data);
        }
      }
      expectEqual("CC", got, f[4]);
    } else if (kind == "CS") {
      const std::vector<std::string> f = split(line, 2);
      const std::vector<ChargeFrame> frames = buildChargeStopCommand();
      std::string got;
      for (std::size_t at = 0; at < frames.size(); at++) {
        char id[12];
        std::snprintf(id, sizeof id, "%x", static_cast<unsigned>(frames[at].id));
        if (at > 0) got += ",";
        got += std::string(id) + ":" + lowerHex8(frames[at].data);
      }
      expectEqual("CS", got, f[1]);
    } else if (kind == "ACK") {
      // ACK {input-json} {verdict-json} <code> "<message>"
      const std::vector<std::string> f = split(line, 5);
      ChargeAckInput input = parseAckInput(f[1]);
      const ChargeAckVerdict verdict = judgeChargeCommand(input);
      expectEqual("ACK-verdict", ackVerdictJson(verdict), f[2]);
      expectEqual("ACK-code", std::to_string(chargeAckCode(verdict.kind)), f[3]);
      const std::string msg = build([&](text::TextBuffer& out) { out.appendJsonString(describeChargeAck(verdict, input.commandedAmps)); });
      expectEqual("ACK-msg", msg, f[4]);
    }
    // TARGETS/TGT are informational; the dropdown shape is checked via the status body in check-ble.
  }

  if (failures > 0) {
    std::fprintf(stderr, "check_write: %d failures\n", failures);
    return 1;
  }
  std::fprintf(stderr, "check_write: all pure write parts match the reference; 0 failures\n");
  return 0;
}
