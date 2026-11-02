#pragma once

// Service mode's two freeze-frame reads, ported from cool-eva src/vcu/lifetime-read.ts,
// freeze-frame-read.ts and freeze-frame-list.ts:
//   - the lifetime statistics: freeze-frame components 51 and 52 off the A8, each asked twice at
//     most
//   - the stored freeze frames: the `18` list of components that hold one, then `17` for each, inside
//     a 12 s budget so the whole read fits the OBD poller's 15 s hold with room to spare
// Both run under the read runner, with the bike safe to service and the poller parked.
//
// cool-eva also reports the worst event-loop delay seen during the read; emcool has no event loop
// to sample in that sense, so it says "loop not sampled", as cool-eva does when it has no sample.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "emcool/async/task.hpp"
#include "emcool/text/text_buffer.hpp"
#include "emcool/vcu/kwp_client.hpp"

namespace emcool::vcu {

// One component's reply as stored: the payload, or why there was none worth keeping.
struct StoredReply {
  int component;
  std::optional<std::string> payloadHex;
  std::optional<std::string> failure;
};
StoredReply toStoredReply(int component, const VcuMultiFrameOutcome& outcome);
int answeredCount(std::span<const StoredReply> replies);
void writeStoredReplyJson(text::TextBuffer& out, const StoredReply& reply);
// "component 51: no-response; component 52: …", or nullopt if every one answered.
std::optional<std::string> describeFailures(std::span<const StoredReply> replies);

std::optional<ArrivalLatency> worseOf(const std::optional<ArrivalLatency>& left,
                                      const std::optional<ArrivalLatency>& right);

struct LifetimeReadResult {
  std::vector<StoredReply> replies;
  std::optional<ArrivalLatency> flowControl;
};
std::string describeMeasurement(const std::optional<ArrivalLatency>& flowControl);

struct FreezeFrameListReport {
  int declaredCount = 0;
  int parsed = 0;
  int padding = 0;
  int outOfRange = 0;
  int duplicate = 0;
  bool truncated = false;
  std::string trailingHex;
};
struct ComponentSelection {
  std::vector<int> components;
  FreezeFrameListReport report;
};
ComponentSelection selectComponentsToRead(const VcuStoredDtcList& list);
std::optional<std::string> describeListDamage(const FreezeFrameListReport& list);
void writeListReportJson(text::TextBuffer& out, const FreezeFrameListReport& list);

struct FreezeFrameReadResult {
  std::string completion;  // complete | budget-spent | no-list | failed | cancelled
  std::optional<std::string> reason;
  std::vector<StoredReply> replies;
  std::vector<int> components;
  std::optional<FreezeFrameListReport> list;
  std::optional<ArrivalLatency> flowControl;
  double elapsedMs = 0;
};
std::string describeFreezeFrameRead(const FreezeFrameReadResult& result);

inline constexpr std::int64_t kFreezeFrameReadBudgetMs = 15000 - 3000;
// worstCaseMultiFrameReadMs(2) and (1): a session open, then the attempts.
inline constexpr std::int64_t kWorstCaseComponentMs = (300 + 10) + 2 * (300 + 400 + 10);

// A read on the bus that the runner hands frames to and can stop.
class OneShotRead {
 public:
  virtual ~OneShotRead() = default;
  virtual bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) = 0;
  virtual void abort(std::string reason) = 0;
};

class LifetimeRead final : public OneShotRead {
 public:
  explicit LifetimeRead(VcuSend send);
  async::Task<LifetimeReadResult> run();
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) override {
    return client_->handleFrame(id, data, arrival);
  }
  void abort(std::string reason) override;

 private:
  std::shared_ptr<VcuKwpClient> client_;
};

class FreezeFrameRead final : public OneShotRead {
 public:
  explicit FreezeFrameRead(VcuSend send);
  async::Task<FreezeFrameReadResult> run();
  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, ArrivalStamp arrival) override {
    return client_->handleFrame(id, data, arrival);
  }
  void abort(std::string reason) override;

 private:
  FreezeFrameReadResult finish(std::string completion, std::optional<std::string> reason);

  std::shared_ptr<VcuKwpClient> client_;
  std::optional<std::string> cancellation_;
  std::int64_t startedAt_ = 0;
  FreezeFrameReadResult result_;
};

// One component, asked again on silence or a discarded reply (cool-eva readOneComponent).
struct ComponentRead {
  StoredReply reply;
  VcuMultiFrameOutcome outcome;
  std::optional<ArrivalLatency> latency;
};
async::Task<ComponentRead> readOneComponent(std::shared_ptr<VcuKwpClient> client, int component, int attempts);

}  // namespace emcool::vcu
