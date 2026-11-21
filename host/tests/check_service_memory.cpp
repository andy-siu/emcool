// What service mode's answers ask of operator new, which on the bike is internal RAM. With Bluetooth
// and Wi-Fi up that has had under 13 KB free and a 5 KB largest block, and a block it can't find
// calls abort(): the bike reboots, and the phone sees Bluetooth drop. A crash dump from 2026-11-20
// shows the Bluetooth task doing just that, asking for 38 KB to copy GET /vcu-write's 20 KB answer
// when Bike information opened.
//
// So every answer the Bike information page and the service-stamp read can bring back is asked for
// here: through ServiceApi, as the diagnostics task asks, and through DashboardEndpoints, as the
// Bluetooth task sends it, a message's worth at a time. The stored readings are as big as a reading
// gets (every freeze-frame component), and the write journal is thousands of records long with the
// damage a power cut leaves. None of it may ask operator new for a big block. And the journal's
// newest records still come back, newest first, the damaged lines skipped and a record after a
// power cut's hole kept.
//
//   check_service_memory

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "emcool/async/loop.hpp"
#include "emcool/async/task.hpp"
#include "emcool/ble/json_reader.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/ble_server/server.hpp"
#include "emcool/obd/bus.hpp"
#include "emcool/vcu/charge_ack_watch.hpp"
#include "emcool/vcu/service_api.hpp"
#include "emcool/vcu/write_runner.hpp"

using namespace emcool;
using namespace emcool::vcu;

namespace {

// The largest block asked of operator new while `trackingAllocations` is on, and the most held at
// once. BigText's memory is malloc'd on the laptop (PSRAM on the bike), so it isn't counted: only
// what would be internal RAM.
bool trackingAllocations = false;
std::size_t largestAllocation = 0;
std::size_t heldNow = 0;
std::size_t mostHeld = 0;

}  // namespace

// Each block carries its size in front, so what is held can be counted.
void* operator new(std::size_t size) {
  auto* block = static_cast<std::size_t*>(std::malloc(size + sizeof(std::max_align_t)));
  if (block == nullptr) throw std::bad_alloc();
  *block = trackingAllocations ? size : 0;
  if (trackingAllocations) {
    largestAllocation = std::max(largestAllocation, size);
    heldNow += size;
    mostHeld = std::max(mostHeld, heldNow);
  }
  return reinterpret_cast<char*>(block) + sizeof(std::max_align_t);
}
void operator delete(void* pointer) noexcept {
  if (pointer == nullptr) return;
  auto* block = reinterpret_cast<std::size_t*>(static_cast<char*>(pointer) - sizeof(std::max_align_t));
  heldNow -= std::min(heldNow, *block);
  std::free(block);
}
void operator delete(void* pointer, std::size_t) noexcept { operator delete(pointer); }

namespace {

// Under the bike's largest free block, and far under a body. The biggest blocks left are the laptop's
// own: ServiceApi::answer's coroutine frame, 3.6 KB here and 1.9 KB in the bike's build, and libc++'s
// 4 KB deque block for the loop's microtasks, which is 512 bytes in the bike's libstdc++.
constexpr std::size_t kMostBlockBytes = 4096;
// And all of it held at once, the coroutine frame included: a 63-component freeze-frame reading is the
// most, its replies held while its body is written.
constexpr std::size_t kMostHeldBytes = 16384;

constexpr std::size_t kJournalRecords = 3000;

void writeFile(const std::string& path, const std::string& text) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}

std::string journalLine(std::size_t at) {
  return "{\"at\":" + std::to_string(at) +
         ",\"clockTrustworthy\":true,\"action\":\"charge-current\",\"status\":\"sent\",\"requested\":" +
         std::to_string(at % 90) + ",\"note\":\"automatic: the pack is warm, so the current steps down to keep it so\"," +
         "\"runningVersion\":\"test\"}";
}

// The journal as a long charge and a power cut leave it. Its newest 12, newest first: 3003 to 2997,
// 2996 found after a hole, then 2995 to 2992.
std::vector<double> seedJournal(const std::string& directory) {
  std::string text;
  for (std::size_t at = 1; at <= 2995; at++) text += journalLine(at) + "\n";
  text += std::string(600, '\0') + "\n";                 // a record lost whole
  text += std::string(300, '\0') + journalLine(2996) + "\n";  // a hole, then a whole record
  text += std::string(100, '\0') + "{\"at\":12,\"act\n";     // a hole, then a torn one
  text += "{not json\n";
  for (std::size_t at = 2997; at <= kJournalRecords + 3; at++) text += journalLine(at) + "\n";
  text += "{\"at\":4000,\"action\":\"charge-cur";  // killed mid-write
  writeFile(directory + "/service-writes.jsonl", text);
  std::vector<double> newest;
  for (std::size_t at = kJournalRecords + 3; at >= 2997; at--) newest.push_back(static_cast<double>(at));
  for (std::size_t at = 2996; newest.size() < 12; at--) newest.push_back(static_cast<double>(at));
  return newest;
}

// "57 02 00 0C …", as the readings are stored.
std::string spacedHex(const std::string& packed) {
  std::string text;
  for (std::size_t at = 0; at + 1 < packed.size(); at += 2) text += (text.empty() ? "" : " ") + packed.substr(at, 2);
  return text;
}

// Every component a freeze-frame read can list, each with a frame: as big as the file gets.
void seedReadings(const std::string& directory) {
  std::string replies;
  std::string components;
  for (int component = 1; component <= 63; component++) {
    char hex[3];
    std::snprintf(hex, sizeof hex, "%02x", component);
    if (component > 1) {
      replies += ",";
      components += ",";
    }
    components += std::to_string(component);
    replies += "{\"component\":" + std::to_string(component) + ",\"payloadHex\":\"" +
               spacedHex(std::string("570200") + hex + "2ec828b75f82f2c3b5394e523683f4d302b5") +
               "\",\"failure\":null}";
  }
  writeFile(directory + "/freeze-frames.json",
            "{\"readAt\":1790000000000,\"source\":\"service\",\"completion\":\"complete\",\"components\":[" + components +
                "],\"replies\":[" + replies +
                "],\"list\":{\"declaredCount\":63,\"parsed\":63,\"padding\":0,\"outOfRange\":0,\"duplicate\":0,"
                "\"truncated\":false,\"trailingHex\":\"\"}}\n");
  writeFile(directory + "/lifetime.json",
            "{\"readAt\":1790000000000,\"source\":\"service\",\"replies\":[{\"component\":51,\"payloadHex\":\"" +
                spacedHex("5700") + "\",\"failure\":null},{\"component\":52,\"payloadHex\":\"" +
                spacedHex("5700003403319458b30f9c9c00b185006db00ff11763") + "\",\"failure\":null}]}\n");
}

class NullRecorder final : public obd::SignalRecorder {
 public:
  void record(std::string_view, double) override {}
};

// The diagnostics task's desk, answering at once on the same loop.
class Desk final : public ble_server::ServiceDesk {
 public:
  Desk(ServiceApi& api, VcuStore& store) : api_(api), store_(store) {}
  // Answered here, but on the bike in the diagnostics task: measured apart, by the first pass.
  bool submit(const ble_server::ServiceRequest& request) override {
    const bool tracking = std::exchange(trackingAllocations, false);
    async::currentLoop().run(0, [&] {
      [](Desk* desk, ServiceRequest asked) -> async::Task<> {
        desk->answer_ = co_await desk->api_.answer(std::move(asked));
      }(this, request).detach();
    });
    trackingAllocations = tracking;
    return true;
  }
  std::optional<ble_server::ServiceAnswer> take() override { return std::exchange(answer_, std::nullopt); }
  void abandon() override { answer_.reset(); }
  VcuStore* store() override { return &store_; }
  std::string displayDirectory() override { return "/vcu-params"; }

 private:
  ServiceApi& api_;
  VcuStore& store_;
  std::optional<ble_server::ServiceAnswer> answer_;
};

class NoStorage final : public ble_server::StorageReport {
 public:
  ble_server::StorageStatus storage() override { return {false, 0, 0, std::nullopt, std::nullopt}; }
};

class NoCodes final : public ble_server::TroubleCodeSource {
 public:
  obd::StoredCodes troubleCodes() override { return {}; }
};

class Silent final : public signals::RideLogSink {
 public:
  void appendReading(std::int64_t, std::int64_t, const signals::SignalDef&, double) override {}
};

// `recent`'s `at`s, from a GET /vcu-write body.
std::vector<double> recentAts(const std::string& body) {
  std::vector<double> ats;
  std::vector<char> scratch(body.size() + 16);
  ble::JsonObjectReader top;
  if (!top.read(body, scratch)) return ats;
  const ble::JsonMember* status = top.find("status");
  if (status == nullptr || status->type != ble::JsonType::kObject) return ats;
  std::vector<char> statusScratch(status->text.size() + 16);
  ble::JsonObjectReader statusReader;
  if (!statusReader.read(status->text, statusScratch)) return ats;
  const ble::JsonMember* recent = statusReader.find("recent");
  std::vector<std::string_view> elements;
  if (recent == nullptr || !ble::jsonArrayElements(recent->text, elements)) return ats;
  for (const std::string_view element : elements) {
    std::vector<char> recordScratch(element.size() + 16);
    ble::JsonObjectReader record;
    if (record.read(element, recordScratch)) ats.push_back(record.find("at")->number);
  }
  return ats;
}

}  // namespace

int main() {
  char pattern[] = "/tmp/emcool-service-memory-XXXXXX";
  const std::string directory = ::mkdtemp(pattern);
  const std::vector<double> newest = seedJournal(directory);
  seedReadings(directory);

  obd::DeadlineOrder order;
  async::Loop loop(order);
  async::setCurrentLoop(&loop);

  ParameterNames names;
  VcuStore store(directory, names);
  ReadingStore readings(directory);
  ServiceContext context;
  context.send = [](std::uint32_t, const std::array<std::uint8_t, 8>&) { return std::optional<std::string>(); };
  context.readSignal = [](std::string_view) { return GateSample{}; };
  context.wallClockMs = [] { return 1790000000000.0; };
  context.store = &store;
  BusLease lease;
  VcuReadRunner runner(context, lease, true);
  NullRecorder recorder;
  ChargeAckWatch ackWatch(recorder);
  WriteContext writeContext;
  writeContext.send = context.send;
  writeContext.enabled = true;
  writeContext.directory = directory;
  writeContext.gate = [] { return ServiceGateVerdict{}; };
  writeContext.readSignal = context.readSignal;
  writeContext.wallClockMs = context.wallClockMs;
  writeContext.monotonicMs = [] { return std::int64_t{0}; };
  writeContext.clockEvidence = [] { return PiClockEvidence{1790000000000.0, std::nullopt, std::nullopt}; };
  writeContext.latestReport = [&store] { return store.loadLatestTableType(); };
  writeContext.version = {std::nullopt, false, true, "test"};
  writeContext.store = &store;
  writeContext.ackWatch = &ackWatch;
  VcuWriteRunner writer(std::move(writeContext), lease);
  ServiceApi api(runner, store, readings, true, [] { return 1790000000000.0; }, &writer);

  Desk desk(api, store);
  Silent rideLog;
  signals::SignalStore signalStore(rideLog);
  std::mutex storeMutex;
  NoStorage noStorage;
  NoCodes noCodes;
  ble_server::DashboardEndpoints endpoints(signalStore, storeMutex, noStorage, noCodes, &desk);

  struct Asked {
    const char* what;
    const char* method;
    const char* path;
    ServiceRequest::Kind kind;
    const char* query;
  };
  const Asked asked[] = {
      // Bike information, as it opens: the write status with its list, the lifetime statistics, the read state.
      {"GET /vcu-write", "GET", "/vcu-write", ServiceRequest::Kind::kWriteStatus, ""},
      {"GET /vcu-write?list=0&detail=…", "GET", "/vcu-write?list=0&detail=MAX_DC_CHG_CURRENT",
       ServiceRequest::Kind::kWriteStatus, "list=0&detail=MAX_DC_CHG_CURRENT"},
      {"GET /lifetime-stats", "GET", "/lifetime-stats", ServiceRequest::Kind::kLifetimeStats, ""},
      {"GET /vcu-read", "GET", "/vcu-read", ServiceRequest::Kind::kReadState, ""},
      // The All tab's freeze frames, and the expected-fault list.
      {"GET /freeze-frames", "GET", "/freeze-frames", ServiceRequest::Kind::kFreezeFrames, ""},
      {"GET /expected-faults", "GET", "/expected-faults", ServiceRequest::Kind::kExpectedFaults, ""},
  };
  for (const Asked& ask : asked) {
    // Through ServiceApi alone: what the diagnostics task asks for.
    ServiceRequest request{ask.kind, {}, {}, {}, {}, {}, {}, ask.query};
    std::optional<ServiceAnswer> answered;
    largestAllocation = heldNow = mostHeld = 0;
    trackingAllocations = true;
    loop.run(0, [&] {
      [](ServiceApi& service, ServiceRequest r, std::optional<ServiceAnswer>& out) -> async::Task<> {
        out = co_await service.answer(std::move(r));
      }(api, request, answered).detach();
    });
    trackingAllocations = false;
    const std::size_t answering = largestAllocation;
    const std::size_t answeringHeld = mostHeld;
    EMCOOL_CHECK(answered && answered->status == 200 && !answered->body.failed() && answered->body.size() > 0,
                 "%s: no answer", ask.what);
    const std::string expected = answered ? std::string(answered->body.view()) : std::string();

    // Through the Bluetooth task's end: begin(), poll() for the answer, then the body a message at a time.
    largestAllocation = heldNow = mostHeld = 0;
    trackingAllocations = true;
    std::optional<ble_server::HttpResponse> response = endpoints.begin({ask.method, ask.path, "", ""}, 0);
    for (int poll = 0; !response && poll < 10; poll++) response = endpoints.poll(0);
    std::string sent;
    while (response && sent.size() < response->bodyLength) {
      const std::span<const char> piece = endpoints.body(
          sent.size(), std::min(ble_server::BleServer::kResponseChunkBytes, response->bodyLength - sent.size()));
      if (piece.empty()) break;
      trackingAllocations = false;
      sent.append(piece.data(), piece.size());
      trackingAllocations = true;
    }
    trackingAllocations = false;
    const std::size_t sending = largestAllocation;
    const std::size_t sendingHeld = mostHeld;
    EMCOOL_CHECK(response && response->status == 200 && sent == expected, "%s through the endpoints: %d, %zu of %zu bytes",
                 ask.what, response ? response->status : 0, sent.size(), expected.size());
    EMCOOL_CHECK(answering <= kMostBlockBytes && sending <= kMostBlockBytes,
                 "%s asked operator new for %zu bytes at once answering, %zu sending; at most %zu", ask.what, answering,
                 sending, kMostBlockBytes);
    EMCOOL_CHECK(answeringHeld <= kMostHeldBytes && sendingHeld <= kMostHeldBytes,
                 "%s held %zu bytes from operator new answering, %zu sending; at most %zu", ask.what, answeringHeld,
                 sendingHeld, kMostHeldBytes);
    std::fprintf(stderr,
                 "check_service_memory: %s is %zu bytes; from operator new, largest block %zu and %zu held answering, "
                 "%zu and %zu sending\n",
                 ask.what, expected.size(), answering, answeringHeld, sending, sendingHeld);

    if (ask.kind == ServiceRequest::Kind::kWriteStatus) {
      const std::vector<double> got = recentAts(expected);
      std::string list;
      for (const double at : got) list += std::to_string(static_cast<long long>(at)) + " ";
      EMCOOL_CHECK(got == newest, "%s: the journal's newest records came back as %s", ask.what, list.c_str());
    }
    if (ask.kind == ServiceRequest::Kind::kFreezeFrames) {
      EMCOOL_CHECK(expected.find("\"component\":63") != std::string::npos && expected.size() > 8192,
                   "%s left out components: %zu bytes", ask.what, expected.size());
    }
    if (ask.kind == ServiceRequest::Kind::kLifetimeStats) {
      EMCOOL_CHECK(expected.find("\"statistics\":") != std::string::npos, "%s has no statistics", ask.what);
    }
  }

  async::setCurrentLoop(nullptr);
  std::filesystem::remove_all(directory);
  std::printf("check_service_memory: %zu answers, none above %zu bytes from operator new; %d failures\n",
              std::size(asked), kMostBlockBytes, emcool::test::failures());
  return emcool::test::failures() == 0 ? 0 : 1;
}
