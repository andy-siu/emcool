#pragma once

// Where service mode keeps its freeze-frame readings and the expected-fault list, ported from
// cool-eva src/vcu/lifetime-store.ts, freeze-frame-store.ts and expected-faults.ts, with the
// bodies of /lifetime-stats, /freeze-frames and /expected-faults. Small files, kept as cool-eva
// keeps them (JSON, here on one line):
//   lifetime.json, lifetime-<ISO>.json            the latest lifetime read, and every read
//   freeze-frames.json, freeze-frames-<ISO>.json  the same for the stored freeze frames
//   expected-faults.json                          the codes this bike is expected to have
// A read that answered less than the stored one doesn't replace it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/readings.hpp"

namespace emcool::vcu {

inline constexpr const char* kLifetimeHowToRead =
    "menu → Service mode → \"Read the lifetime battery statistics\", parked, with the drive down or plugged in";
// cool-eva's names a script run with its service stopped; emcool has no service to stop.
inline constexpr const char* kLifetimeHowToReadWithServiceStopped =
    "set OBD_ENABLED 1 and SERVICE_MODE_ENABLED 1 on the serial console, restart, and read it from the dashboard";
inline constexpr const char* kFreezeFramesHowToRead =
    "menu → Service mode → \"Read the stored freeze frames\", parked, with the drive down or plugged in";

struct StoredLifetimeRead {
  double readAt;
  std::string source;  // "service"
  std::vector<StoredReply> replies;
};

struct StoredFreezeFrameRead {
  double readAt;
  std::string source;
  std::string completion;
  std::vector<int> components;
  std::vector<StoredReply> replies;
  std::optional<FreezeFrameListReport> list;
};

struct ExpectedFault {
  int component;
  int symptom;
};
struct ExpectedFaults {
  std::vector<ExpectedFault> entries;
  std::optional<double> updatedAt;
};
bool isValidFaultKey(double component, double symptom);
void writeExpectedFaultsJson(text::TextBuffer& out, const ExpectedFaults& expected);

class ReadingStore {
 public:
  explicit ReadingStore(std::string directory) : directory_(std::move(directory)) {}

  struct Written {
    bool stored;
    std::string reason;
    int answered;
  };
  Written writeLifetimeRead(const StoredLifetimeRead& read);
  Written writeFreezeFrameRead(const StoredFreezeFrameRead& read);

  // /lifetime-stats and /freeze-frames.
  std::string lifetimeStatsBody();
  std::string freezeFramesBody();

  ExpectedFaults loadExpectedFaults();
  ExpectedFaults setExpectedFault(ExpectedFault fault, bool expected, double wallClockMs);

 private:
  std::string path(const std::string& name) const { return directory_ + "/" + name; }
  bool makeDirectory();
  std::optional<StoredLifetimeRead> loadLifetimeRead();
  std::optional<StoredFreezeFrameRead> loadFreezeFrameRead();
  bool writeFile(const std::string& path, const std::string& text);

  std::string directory_;
};

}  // namespace emcool::vcu
