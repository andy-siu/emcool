// Replays the runs written by tools/generate-golden-pack-temperature.mts — the reference's own
// pack-temperature routing on scripted 0x200/0x660 sequences — through the C++ decoder and
// PackTemperatureRouter, and requires the same values for every frame and the same number of
// errors, warnings and info lines for every run.
//
//   check_pack_temperature <golden pack-temperature file>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "emcool/can/frame_decoder.hpp"
#include "emcool/can/pack_temperature.hpp"
#include "emcool/log.hpp"

int main(int argumentCount, char** arguments) {
  if (argumentCount != 2) {
    std::fprintf(stderr, "usage: check_pack_temperature <golden file>\n");
    return 2;
  }
  std::ifstream golden(arguments[1]);
  if (!golden) {
    std::fprintf(stderr, "check_pack_temperature: cannot open %s\n", arguments[1]);
    return 2;
  }
  std::unique_ptr<emcool::can::PackTemperatureRouter> router;
  std::unique_ptr<emcool::can::FrameDecoder> decoder;
  emcool::LogCounts atRunStart;
  int run = 0;
  unsigned long long frames = 0;
  int reported = 0;

  for (std::string line; std::getline(golden, line);) {
    std::istringstream fields(line);
    std::string first;
    fields >> first;
    if (first == "run") {
      int custom = 0;
      fields >> custom;
      router = std::make_unique<emcool::can::PackTemperatureRouter>(custom == 1);
      decoder = std::make_unique<emcool::can::FrameDecoder>();
      atRunStart = emcool::logCountsFor("bms");
      run++;
      continue;
    }
    if (first == "logs") {
      unsigned errors = 0, warnings = 0, infos = 0;
      fields >> errors >> warnings >> infos;
      const emcool::LogCounts now = emcool::logCountsFor("bms");
      EMCOOL_CHECK(now.error - atRunStart.error == errors && now.warning - atRunStart.warning == warnings &&
                       now.info - atRunStart.info == infos,
                   "run %d logged %u/%u/%u (errors/warnings/infos), the reference %u/%u/%u", run,
                   now.error - atRunStart.error, now.warning - atRunStart.warning, now.info - atRunStart.info, errors,
                   warnings, infos);
      continue;
    }
    const std::int64_t monotonicMs = std::strtoll(first.c_str(), nullptr, 10);
    std::string idText, payloadHex;
    fields >> idText >> payloadHex;
    const auto id = static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 16));
    std::vector<std::uint8_t> payload;
    for (std::size_t offset = 0; offset + 1 < payloadHex.size(); offset += 2) {
      payload.push_back(static_cast<std::uint8_t>(std::strtoul(payloadHex.substr(offset, 2).c_str(), nullptr, 16)));
    }
    emcool::can::DecodedValues decoded;
    decoder->decode(id, payload, decoded);
    router->resolve(id, payload, decoded, monotonicMs);
    frames++;

    std::vector<std::pair<std::string, double>> expected;
    for (std::string pair; fields >> pair;) {
      const std::size_t equals = pair.find('=');
      expected.emplace_back(pair.substr(0, equals), std::strtod(pair.c_str() + equals + 1, nullptr));
    }
    bool matches = expected.size() == decoded.size();
    for (std::size_t index = 0; matches && index < decoded.size(); index++) {
      matches = expected[index].first == decoded[index].key && expected[index].second == decoded[index].value;
    }
    if (!matches) {
      emcool::test::failures()++;
      if (reported++ < 10) {
        std::string actual;
        for (const auto& value : decoded) actual += " " + std::string(value.key) + "=" + std::to_string(value.value);
        std::fprintf(stderr, "MISMATCH run %d\n  golden:%s\n  c++   :%s\n", run, line.c_str(), actual.c_str());
      }
    }
  }
  std::printf("check_pack_temperature: %d runs, %llu frames, %d failures\n", run, frames, emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
