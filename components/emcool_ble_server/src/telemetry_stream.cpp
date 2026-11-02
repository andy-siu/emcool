#include "emcool/ble_server/telemetry_stream.hpp"

#include "emcool/text/text_buffer.hpp"

namespace emcool::ble_server {

namespace {

using text::TextBuffer;

constexpr std::size_t kClosingBytes = 2;

void beginMessage(TextBuffer& text, const char* type, std::int64_t wallClockMs) {
  text.append("{\"type\":\"");
  text.append(type);
  text.append("\",\"ts\":");
  text.appendInteger(wallClockMs);
  text.append(",\"signals\":{");
}

// Appends `"key":{"value":…,"unit":…,"group":…,"ts":…}`, or rolls back and returns false.
bool appendEntry(TextBuffer& text, bool first, const signals::SignalDef& signal, const signals::LiveValue& live) {
  const std::size_t before = text.length();
  if (!first) text.append(",");
  text.appendJsonString(signal.key);
  text.append(":{\"value\":");
  text.appendNumber(live.value);
  text.append(",\"unit\":");
  text.appendJsonString(signal.unit);
  text.append(",\"group\":");
  text.appendJsonString(signal.group);
  text.append(",\"ts\":");
  text.appendInteger(live.wallClockMs);
  text.append("}");
  if (text.overflowed()) {
    text.truncate(before);
    return false;
  }
  return true;
}

std::size_t finish(std::span<char> out, std::size_t length) {
  TextBuffer closing(out.data(), out.size(), length);
  closing.append("}}");
  return closing.overflowed() ? 0 : closing.length();
}

}  // namespace

std::size_t TelemetryStream::nextSnapshotChunk(const signals::SignalStore& store, std::int64_t wallClockMs,
                                               std::span<char> out) {
  if (!snapshotting_ || out.size() <= kClosingBytes) return 0;
  const auto registry = signals::signalRegistry();
  TextBuffer text(out.data(), out.size() - kClosingBytes);
  beginMessage(text, "snapshot", wallClockMs);
  std::size_t entries = 0;
  while (snapshotCursor_ < registry.size()) {
    if (store.seenAt(snapshotCursor_)) {
      if (!appendEntry(text, entries == 0, registry[snapshotCursor_], store.liveAt(snapshotCursor_))) {
        // Can't happen at these sizes, but a signal that fits no chunk must not stall the snapshot.
        if (entries == 0) snapshotCursor_++;
        break;
      }
      entries++;
    }
    snapshotCursor_++;
  }
  if (snapshotCursor_ >= registry.size()) {
    snapshotting_ = false;
  }
  // An empty store still gets one (empty) snapshot, so a new subscriber knows it's live.
  return text.overflowed() ? 0 : finish(out, text.length());
}

std::size_t TelemetryStream::nextPatch(signals::SignalStore& store, std::int64_t wallClockMs, std::span<char> out) {
  if (!store.anyChanged() || out.size() <= kClosingBytes) return 0;
  const auto registry = signals::signalRegistry();
  TextBuffer text(out.data(), out.size() - kClosingBytes);
  beginMessage(text, "patch", wallClockMs);
  std::size_t entries = 0;
  for (std::size_t index = 0; index < registry.size(); index++) {
    if (!store.changedAt(index)) continue;
    if (!appendEntry(text, entries == 0, registry[index], store.liveAt(index))) break;
    store.clearChanged(index);
    entries++;
  }
  if (entries == 0) return 0;
  return text.overflowed() ? 0 : finish(out, text.length());
}

}  // namespace emcool::ble_server
