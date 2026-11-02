#include "emcool/vcu/readings.hpp"

#include <set>

#include "emcool/log.hpp"
#include "emcool/obd/freeze_frame.hpp"
#include "emcool/obd/lifetime.hpp"
#include "emcool/vcu/js.hpp"

namespace emcool::vcu {

namespace {

constexpr int kAttemptsPerComponent = 2;

std::optional<ArrivalLatency> latencyOf(const VcuMultiFrameOutcome& outcome) {
  using Status = VcuMultiFrameOutcome::Status;
  if (outcome.status == Status::kReply || outcome.status == Status::kNoResponse || outcome.status == Status::kAbandoned) {
    return outcome.flowControlLatency;
  }
  return std::nullopt;
}

bool shouldRetry(const VcuMultiFrameOutcome& outcome) {
  return outcome.status == VcuMultiFrameOutcome::Status::kNoResponse ||
         outcome.status == VcuMultiFrameOutcome::Status::kAbandoned;
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

}  // namespace

StoredReply toStoredReply(int component, const VcuMultiFrameOutcome& outcome) {
  if (outcome.status != VcuMultiFrameOutcome::Status::kReply) {
    return {component, std::nullopt, std::string(multiFrameStatusName(outcome.status))};
  }
  const std::string payloadHex = toHex(outcome.payload);
  if (outcome.reply.kind == VcuMultiFrameReply::Kind::kPositive) {
    if (obd::isNoStoredRecordReply(outcome.payload)) return {component, payloadHex, std::nullopt};
    const obd::FreezeFrameResponse decoded = obd::decodeFreezeFrameResponse(outcome.payload, component);
    if (decoded.kind == obd::FreezeFrameResponse::Kind::kFrame) return {component, payloadHex, std::nullopt};
    if (decoded.kind == obd::FreezeFrameResponse::Kind::kComponentMismatch) {
      return {component, payloadHex,
              "component-mismatch: asked " + std::to_string(decoded.requested) + ", answered " +
                  std::to_string(decoded.received)};
    }
    // A refusal can't come back positive, so what's left carries a reason.
    const std::string reason = decoded.kind == obd::FreezeFrameResponse::Kind::kUnrecognised
                                   ? decoded.reason
                                   : std::string(obd::freezeFrameKindName(decoded.kind));
    return {component, payloadHex, "unrecognised: " + reason};
  }
  if (outcome.reply.kind == VcuMultiFrameReply::Kind::kRefused) {
    char code[8];
    std::snprintf(code, sizeof code, "%02x", outcome.reply.negativeResponseCode);
    return {component, payloadHex, "refused: " + outcome.reply.description + " (NRC 0x" + code + ")"};
  }
  return {component, payloadHex, "unrecognised: " + outcome.reply.reason};
}

int answeredCount(std::span<const StoredReply> replies) {
  int answered = 0;
  for (const StoredReply& reply : replies) {
    if (!reply.failure) answered++;
  }
  return answered;
}

void writeStoredReplyJson(text::TextBuffer& out, const StoredReply& reply) {
  out.append("{\"component\":");
  out.appendInteger(reply.component);
  out.append(",\"payloadHex\":");
  writeOptionalString(out, reply.payloadHex);
  out.append(",\"failure\":");
  writeOptionalString(out, reply.failure);
  out.append("}");
}

std::optional<std::string> describeFailures(std::span<const StoredReply> replies) {
  std::string text;
  for (const StoredReply& reply : replies) {
    if (!reply.failure) continue;
    if (!text.empty()) text += "; ";
    text += "component " + std::to_string(reply.component) + ": " + *reply.failure;
  }
  if (text.empty()) return std::nullopt;
  return text;
}

std::optional<ArrivalLatency> worseOf(const std::optional<ArrivalLatency>& left,
                                      const std::optional<ArrivalLatency>& right) {
  if (!left) return right;
  if (!right) return left;
  if (!left->known || !right->known) return left->known ? right : left;
  return right->ms > left->ms ? right : left;
}

std::string describeMeasurement(const std::optional<ArrivalLatency>& flowControl) {
  const std::string loop = "loop not sampled";
  if (!flowControl) return "no flow control was needed · " + loop;
  if (flowControl->known) {
    return "flow control " + jsToFixed(flowControl->ms, 1) + " ms after the kernel saw the First Frame · " + loop;
  }
  return "flow-control latency unmeasured (" + flowControl->reason + ") · " + loop;
}

ComponentSelection selectComponentsToRead(const VcuStoredDtcList& list) {
  ComponentSelection selection;
  std::set<int> seen;
  for (const StoredDtcRecord& record : list.records) {
    if (record.code == 0 && record.status == 0) continue;
    if (record.code < obd::kFreezeFrameMinComponent || record.code > obd::kFreezeFrameMaxComponent) {
      selection.report.outOfRange++;
      continue;
    }
    if (!seen.insert(record.code).second) {
      selection.report.duplicate++;
      continue;
    }
    selection.components.push_back(record.code);
  }
  selection.report.declaredCount = list.declaredCount;
  selection.report.parsed = static_cast<int>(list.records.size());
  selection.report.padding = list.paddingRecords;
  selection.report.truncated = list.truncated;
  selection.report.trailingHex = list.trailingHex;
  return selection;
}

std::optional<std::string> describeListDamage(const FreezeFrameListReport& list) {
  std::vector<std::string> damage;
  if (!list.trailingHex.empty()) {
    int bytes = 1;
    for (const char character : list.trailingHex) bytes += character == ' ' ? 1 : 0;
    damage.push_back(std::to_string(bytes) + " byte(s) after the last whole record");
  }
  if (list.truncated) {
    damage.push_back("declared " + std::to_string(list.declaredCount) + " records and sent " +
                     std::to_string(list.parsed));
  }
  if (list.outOfRange != 0) damage.push_back(std::to_string(list.outOfRange) + " record(s) outside 1…63");
  if (damage.empty()) return std::nullopt;
  std::string text;
  for (const std::string& item : damage) text += (text.empty() ? "" : ", ") + item;
  return text;
}

void writeListReportJson(text::TextBuffer& out, const FreezeFrameListReport& list) {
  out.append("{\"declaredCount\":");
  out.appendInteger(list.declaredCount);
  out.append(",\"parsed\":");
  out.appendInteger(list.parsed);
  out.append(",\"padding\":");
  out.appendInteger(list.padding);
  out.append(",\"outOfRange\":");
  out.appendInteger(list.outOfRange);
  out.append(",\"duplicate\":");
  out.appendInteger(list.duplicate);
  out.append(list.truncated ? ",\"truncated\":true,\"trailingHex\":" : ",\"truncated\":false,\"trailingHex\":");
  out.appendJsonString(list.trailingHex);
  out.append("}");
}

std::string describeFreezeFrameRead(const FreezeFrameReadResult& result) {
  const std::string scale = std::to_string(answeredCount(result.replies)) + "/" + std::to_string(result.components.size()) +
                            " listed component(s) answered in " + jsToFixed(result.elapsedMs / 1000, 1) + " s · " +
                            describeMeasurement(result.flowControl);
  const std::string reason = result.reason.value_or("null");
  if (result.completion == "complete") return "freeze frames: read " + scale;
  if (result.completion == "budget-spent") return "freeze frames: TRUNCATED, " + scale + " — " + reason;
  if (result.completion == "no-list") return "freeze frames: NO LIST — " + reason;
  if (result.completion == "failed") return "freeze frames: FAILED after " + scale + " — " + reason;
  return "freeze frames: STOPPED after " + scale + " — " + reason;
}

async::Task<ComponentRead> readOneComponent(std::shared_ptr<VcuKwpClient> client, int component, int attempts) {
  const VcuMultiFrameRequest request{VcuMultiFrameRequest::Kind::kReadFreezeFrame, component};
  VcuMultiFrameOutcome outcome = co_await client->multiFrameRead(Micro::kA8, request);
  std::optional<ArrivalLatency> latency = latencyOf(outcome);
  for (int attempt = 1; attempt < attempts && shouldRetry(outcome); attempt++) {
    logInfo("lifetime", "component %d came back %s — asking once more", component, multiFrameStatusName(outcome.status));
    outcome = co_await client->multiFrameRead(Micro::kA8, request);
    latency = worseOf(latency, latencyOf(outcome));
  }
  co_return ComponentRead{toStoredReply(component, outcome), outcome, latency};
}

// --- The lifetime statistics -----------------------------------------------------------------

LifetimeRead::LifetimeRead(VcuSend send) : client_(std::make_shared<VcuKwpClient>(std::move(send))) {}

void LifetimeRead::abort(std::string reason) {
  logInfo("lifetime", "read aborted — %s", reason.c_str());
  client_->stop();
}

async::Task<LifetimeReadResult> LifetimeRead::run() {
  const std::shared_ptr<VcuKwpClient> client = client_;
  LifetimeReadResult result;
  for (const int component : {obd::kLifetimePackState, obd::kLifetimeCounters}) {
    const ComponentRead read = co_await readOneComponent(client, component, kAttemptsPerComponent);
    result.replies.push_back(read.reply);
    result.flowControl = worseOf(result.flowControl, read.latency);
  }
  client->stop();
  co_return result;
}

// --- The stored freeze frames ----------------------------------------------------------------

FreezeFrameRead::FreezeFrameRead(VcuSend send)
    : client_(std::make_shared<VcuKwpClient>(std::move(send))), startedAt_(async::currentLoop().now()) {}

void FreezeFrameRead::abort(std::string reason) {
  if (!cancellation_) cancellation_ = reason;
  logInfo("freeze-frame", "read aborted — %s", reason.c_str());
  client_->stop();
}

FreezeFrameReadResult FreezeFrameRead::finish(std::string completion, std::optional<std::string> reason) {
  FreezeFrameReadResult result = std::move(result_);
  result.completion = std::move(completion);
  result.reason = std::move(reason);
  result.elapsedMs = jsRound(static_cast<double>(async::currentLoop().now() - startedAt_));
  return result;
}

async::Task<FreezeFrameReadResult> FreezeFrameRead::run() {
  const std::shared_ptr<VcuKwpClient> client = client_;
  const VcuMultiFrameOutcome listed =
      co_await client->multiFrameRead(Micro::kA8, {VcuMultiFrameRequest::Kind::kListStoredDtcs});
  std::optional<std::string> failure;
  std::vector<std::uint8_t> body;
  using Status = VcuMultiFrameOutcome::Status;
  switch (listed.status) {
    case Status::kReply:
      if (listed.reply.kind == VcuMultiFrameReply::Kind::kPositive) {
        body = listed.reply.body;
      } else {
        const VcuMultiFrameReply reply = decodeMultiFrameReply(listed.payload, 0x58);
        failure = reply.kind == VcuMultiFrameReply::Kind::kRefused
                      ? "18 ReadDTCByStatus refused: " + reply.description
                      : "18 ReadDTCByStatus: " +
                            (reply.kind == VcuMultiFrameReply::Kind::kUnrecognised ? reply.reason : "unusable reply");
      }
      break;
    case Status::kNoResponse:
      failure = std::string("18 ReadDTCByStatus: no reply, stalled at ") + transferStageName(listed.stage);
      break;
    case Status::kAbandoned:
      failure = "18 ReadDTCByStatus: reply discarded as unusable — " + listed.reason;
      break;
    case Status::kNoSession:
      failure = "18 ReadDTCByStatus: " + listed.reason;
      break;
    case Status::kCancelled:
      failure = "18 ReadDTCByStatus: cancelled — " + listed.reason;
      break;
    case Status::kNotSent:
      failure = "18 ReadDTCByStatus: never reached the bus — " + listed.reason;
      break;
  }
  if (failure) {
    client->stop();
    if (cancellation_) co_return finish("cancelled", cancellation_);
    co_return finish("no-list", failure);
  }
  const VcuStoredDtcList decoded = decodeStoredDtcList(body);
  const ComponentSelection selection = selectComponentsToRead(decoded);
  logInfo("freeze-frame", "0x18 listed %d record(s) — asking about %zu (%d padding, %d out of range, %d duplicate)",
          decoded.declaredCount, selection.components.size(), selection.report.padding, selection.report.outOfRange,
          selection.report.duplicate);
  result_.components = selection.components;
  result_.list = selection.report;
  for (const int component : selection.components) {
    if (cancellation_) break;
    if (async::currentLoop().now() - startedAt_ + kWorstCaseComponentMs > kFreezeFrameReadBudgetMs) break;
    const ComponentRead read = co_await readOneComponent(client, component, kAttemptsPerComponent);
    result_.replies.push_back(read.reply);
    result_.flowControl = worseOf(result_.flowControl, read.latency);
  }
  client->stop();
  if (cancellation_) co_return finish("cancelled", cancellation_);
  if (result_.replies.size() < selection.components.size()) {
    const double left = jsRound(static_cast<double>(kFreezeFrameReadBudgetMs) -
                                static_cast<double>(async::currentLoop().now() - startedAt_));
    const std::string reason = "stopped after " + std::to_string(result_.replies.size()) + " of " +
                               std::to_string(selection.components.size()) +
                               " component(s): another read could take " + std::to_string(kWorstCaseComponentMs) +
                               " ms and the " + std::to_string(kFreezeFrameReadBudgetMs) + " ms budget had " +
                               jsNumber(left) + " ms left";
    logWarning("freeze-frame", "%s", reason.c_str());
    co_return finish("budget-spent", reason);
  }
  co_return finish("complete", std::nullopt);
}

}  // namespace emcool::vcu
