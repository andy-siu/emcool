// Replays the corpus written by tools/generate-golden.mts — frames, each with the values
// cool-eva's TypeScript decoded from it — through the C++ FrameDecoder, and requires the same
// keys in the same order with bit-identical values. Then feeds the decoded values through a
// SignalStore and requires that no location-bearing signal ever reaches the ride log.
//
//   check_decoders <golden file>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/can/frame_decoder.hpp"
#include "emcool/signals/ride_log_policy.hpp"
#include "emcool/signals/signal_store.hpp"

namespace {

struct ExpectedValue {
  std::string key;
  double value;
};

struct GoldenFrame {
  std::uint32_t id = 0;
  std::vector<std::uint8_t> payload;
  std::vector<ExpectedValue> expected;
};

class RideLogRecorder : public emcool::signals::RideLogSink {
 public:
  void appendReading(std::int64_t, std::int64_t, const emcool::signals::SignalDef& signal, double) override {
    appendedCount++;
    if (emcool::signals::isExcludedFromRideLog(signal.key)) {
      excludedKeysAppended.insert(signal.key);
    }
  }

  std::uint64_t appendedCount = 0;
  std::set<std::string> excludedKeysAppended;
};

bool parseLine(const std::string& line, GoldenFrame& frame);
bool sameValue(double expected, double actual);
std::string describe(const emcool::can::DecodedValues& values);

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_decoders <golden file from tools/generate-golden.mts>\n");
    return 2;
  }
  std::ifstream golden(arguments[1]);
  if (!golden) {
    std::fprintf(stderr, "check_decoders: cannot open %s\n", arguments[1]);
    return 2;
  }

  emcool::can::FrameDecoder decoder;
  RideLogRecorder rideLog;
  emcool::signals::SignalStore store(rideLog);
  std::set<std::string> excludedKeysDecoded;
  std::uint64_t frameCount = 0;
  std::uint64_t valueCount = 0;
  std::size_t mostValuesInOneFrame = 0;
  int reportedMismatches = 0;
  constexpr int kMaxReportedMismatches = 20;

  std::string line;
  std::uint64_t lineNumber = 0;
  while (std::getline(golden, line)) {
    lineNumber++;
    GoldenFrame frame;
    if (!parseLine(line, frame)) {
      std::fprintf(stderr, "check_decoders: unparseable line %llu: %s\n",
                   static_cast<unsigned long long>(lineNumber), line.c_str());
      return 2;
    }
    frameCount++;

    emcool::can::DecodedValues actual;
    decoder.decode(frame.id, frame.payload, actual);
    EMCOOL_CHECK(!actual.overflowed(), "line %llu: id 0x%03x yielded more than %zu values",
                 static_cast<unsigned long long>(lineNumber), frame.id, emcool::can::DecodedValues::kCapacity);
    mostValuesInOneFrame = std::max(mostValuesInOneFrame, actual.size());

    bool matches = actual.size() == frame.expected.size();
    for (std::size_t index = 0; matches && index < actual.size(); index++) {
      matches = frame.expected[index].key == actual[index].key &&
                sameValue(frame.expected[index].value, actual[index].value);
    }
    if (!matches) {
      emcool::test::failures()++;
      if (reportedMismatches < kMaxReportedMismatches) {
        reportedMismatches++;
        std::fprintf(stderr, "MISMATCH line %llu (id 0x%03x)\n  golden: %s\n  c++   : %s\n",
                     static_cast<unsigned long long>(lineNumber), frame.id, line.c_str(),
                     describe(actual).c_str());
      }
    }

    for (const emcool::can::DecodedValue& decoded : actual) {
      valueCount++;
      EMCOOL_CHECK(emcool::signals::findSignal(decoded.key).has_value(),
                   "decoder key %s is not in the registry", decoded.key);
      if (emcool::signals::isExcludedFromRideLog(decoded.key)) {
        excludedKeysDecoded.insert(decoded.key);
      }
      store.record(decoded.key, decoded.value, static_cast<std::int64_t>(frameCount),
                   static_cast<std::int64_t>(frameCount));
    }
  }

  // Guards the privacy check against passing vacuously: the corpus has to contain fixes.
  for (const char* key : {"gps_lat", "gps_lon", "gps_course_deg", "gps_altitude_m"}) {
    EMCOOL_CHECK(excludedKeysDecoded.count(key) == 1, "the corpus never decoded %s, so its exclusion went untested",
                 key);
  }
  for (const std::string& key : rideLog.excludedKeysAppended) {
    EMCOOL_CHECK(false, "%s reached the ride log", key.c_str());
  }
  EMCOOL_CHECK(store.unknownKeyCount() == 0, "%u readings had unregistered keys", store.unknownKeyCount());

  std::printf("check_decoders: %llu frames, %llu values (at most %zu per frame), %llu ride-log appends, %d failures\n",
              static_cast<unsigned long long>(frameCount), static_cast<unsigned long long>(valueCount),
              mostValuesInOneFrame, static_cast<unsigned long long>(rideLog.appendedCount),
              emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}

namespace {

bool parseHexByte(const char* text, std::uint8_t& byte) {
  char digits[3] = {text[0], text[1], '\0'};
  char* end = nullptr;
  const unsigned long value = std::strtoul(digits, &end, 16);
  if (end != digits + 2) return false;
  byte = static_cast<std::uint8_t>(value);
  return true;
}

bool parseLine(const std::string& line, GoldenFrame& frame) {
  std::istringstream fields(line);
  std::string idText;
  std::string payloadText;
  if (!(fields >> idText >> payloadText)) return false;
  frame.id = static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 16));
  if (payloadText != "-") {
    if (payloadText.size() % 2 != 0) return false;
    for (std::size_t offset = 0; offset < payloadText.size(); offset += 2) {
      std::uint8_t byte = 0;
      if (!parseHexByte(payloadText.c_str() + offset, byte)) return false;
      frame.payload.push_back(byte);
    }
  }
  std::string pair;
  while (fields >> pair) {
    const std::size_t equals = pair.find('=');
    if (equals == std::string::npos) return false;
    const std::string valueText = pair.substr(equals + 1);
    char* end = nullptr;
    const double value = std::strtod(valueText.c_str(), &end);
    if (end != valueText.c_str() + valueText.size()) return false;
    frame.expected.push_back({pair.substr(0, equals), value});
  }
  return true;
}

// Bit-identical, except that NaN equals NaN and -0 equals 0 (JSON and String() lose the sign).
bool sameValue(double expected, double actual) {
  if (std::isnan(expected) || std::isnan(actual)) {
    return std::isnan(expected) && std::isnan(actual);
  }
  return expected == actual;
}

std::string describe(const emcool::can::DecodedValues& values) {
  std::string text;
  char number[64];
  for (const emcool::can::DecodedValue& decoded : values) {
    std::snprintf(number, sizeof number, "%.17g", decoded.value);
    text += ' ';
    text += decoded.key;
    text += '=';
    text += number;
  }
  return text.empty() ? "(nothing)" : text;
}

}  // namespace
