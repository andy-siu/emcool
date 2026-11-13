#include "emcool/ride_log/ride_log.hpp"

#include <cstring>
#include <string_view>

#include "emcool/log.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::ride_log {

using text::TextBuffer;

namespace {

constexpr const char* kTag = "ride-log";
// A row is `[ts,"key",value,seq]\n`: 13-digit ts, a registry key, a shortest-form double and a
// sequence number fit with room to spare.
constexpr std::size_t kMaxRowBytes = 160;

}  // namespace

RideLog::RideLog(const SessionId& session, std::span<SegmentBuffer> buffers, SegmentQueue& queue,
                 const clock::ClockTrustSource& clockTrust)
    : session_(session), buffers_(buffers), queue_(queue), clockTrust_(clockTrust) {}

void RideLog::appendReading(std::int64_t wallClockMs, std::int64_t monotonicMs, const signals::SignalDef& signal,
                            double value) {
  // Every reading takes a sequence number, kept or not, so a dropped one leaves a gap in `seq`.
  const std::uint64_t sequence = nextSequence_++;
  const clock::ClockTrust trust = clockTrust_.trust();

  // A segment carries one trust, so a change seals the rows stamped under the old one.
  if (active_ != nullptr && active_->trust != trust) {
    if (active_->rowCount > 0) {
      seal();
    } else {
      beginSegment(*active_, trust, monotonicMs);
    }
  }
  if (active_ == nullptr) {
    SegmentBuffer* buffer = findFreeBuffer();
    if (buffer == nullptr || !beginSegment(*buffer, trust, monotonicMs)) {
      counters_.readingsDropped++;
      return;
    }
  }

  char rowBytes[kMaxRowBytes];
  TextBuffer row(rowBytes, sizeof rowBytes);
  row.append("[");
  row.appendInteger(wallClockMs);
  row.append(",");
  row.appendJsonString(signal.key);
  row.append(",");
  row.appendNumber(value);
  row.append(",");
  row.appendUnsigned(sequence);
  row.append("]\n");
  if (row.overflowed()) {
    counters_.readingsDropped++;
    logError(kTag, "a row for %s did not fit in %zu bytes; dropped", signal.key, kMaxRowBytes);
    return;
  }

  if (active_->payloadLength + row.length() > kMaxPayloadBytes) {
    seal();
    SegmentBuffer* buffer = findFreeBuffer();
    if (buffer == nullptr || !beginSegment(*buffer, trust, monotonicMs)) {
      counters_.readingsDropped++;
      return;
    }
  }
  std::memcpy(active_->payload() + active_->payloadLength, rowBytes, row.length());
  active_->payloadLength += row.length();
  active_->rowCount++;
  counters_.readingsAppended++;

  sealIfParking(signal, value, monotonicMs);
}

void RideLog::tick(std::int64_t monotonicMs) {
  if (active_ != nullptr && active_->rowCount > 0 && monotonicMs - active_->openedAtMonotonicMs >= kMaxSegmentAgeMs) {
    seal();
  }
}

void RideLog::seal() {
  if (active_ == nullptr || active_->rowCount == 0) {
    return;
  }
  SegmentBuffer& sealed = *active_;
  active_ = nullptr;
  sealed.heldByWriter.store(true, std::memory_order_release);
  if (!queue_.submit(sealed)) {
    sealed.heldByWriter.store(false, std::memory_order_release);
    counters_.segmentsLost++;
    counters_.readingsDropped += sealed.rowCount;
    logError(kTag, "the writer would not take a sealed segment; %zu readings lost", sealed.rowCount);
    return;
  }
  counters_.segmentsSealed++;
}

// The segment's first line: the v3 header without the `signals` map — the writer
// puts every signal's unit, group and source in the records that open each file instead.
bool RideLog::beginSegment(SegmentBuffer& buffer, clock::ClockTrust trust, std::int64_t monotonicMs) {
  TextBuffer header(buffer.payload(), kMaxPayloadBytes);
  header.append("{\"v\":3,\"session\":");
  header.appendJsonString(session_.hex.data());
  header.append(",\"trust\":");
  header.appendJsonString(clock::clockTrustName(trust));
  header.append("}\n");
  if (header.overflowed()) {
    logError(kTag, "a segment header did not fit");
    return false;
  }
  buffer.payloadLength = header.length();
  buffer.rowCount = 0;
  buffer.trust = trust;
  buffer.openedAtMonotonicMs = monotonicMs;
  active_ = &buffer;
  return true;
}

SegmentBuffer* RideLog::findFreeBuffer() {
  for (SegmentBuffer& buffer : buffers_) {
    if (&buffer != active_ && !buffer.heldByWriter.load(std::memory_order_acquire)) {
      return &buffer;
    }
  }
  return nullptr;
}

// Seal-on-park: seal when vehicle_state_can enters 60 from something else, at most
// once per 5 s in case the decode ever chatters about what 60 means.
void RideLog::sealIfParking(const signals::SignalDef& signal, double value, std::int64_t monotonicMs) {
  if (std::string_view(signal.key) != "vehicle_state_can") {
    return;
  }
  const std::optional<double> previous = lastVehicleState_;
  lastVehicleState_ = value;
  if (!previous || *previous == kParkedVehicleState || value != kParkedVehicleState) {
    return;
  }
  if (lastParkSealAtMs_ && monotonicMs - *lastParkSealAtMs_ < kMinMsBetweenParkSeals) {
    return;
  }
  lastParkSealAtMs_ = monotonicMs;
  counters_.parkSeals++;
  seal();
}

}  // namespace emcool::ride_log
