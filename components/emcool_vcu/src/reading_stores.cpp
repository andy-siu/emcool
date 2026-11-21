#include "emcool/vcu/reading_stores.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <utility>

#include "emcool/ble/json_reader.hpp"
#include "emcool/log.hpp"
#include "emcool/obd/lifetime.hpp"
#include "emcool/vcu/snapshot.hpp"
#include "emcool/vcu/store.hpp"

namespace emcool::vcu {

namespace {

// Measured first, so the block is exactly the text's size, and in PSRAM: a freeze-frame reading
// runs to several KB, more than the bike's internal RAM can be asked for at once.
template <typename Write>
text::BigText build(Write write) {
  return text::BigText::build(write);
}

// A file read whole, in PSRAM; nullopt if it isn't there or there's no memory for it.
std::optional<text::BigText> readWholeFile(const std::string& path, const char* tag) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    if (errno != ENOENT) logWarning(tag, "could not read %s: %s", path.c_str(), std::strerror(errno));
    return std::nullopt;
  }
  long size = -1;
  if (std::fseek(file, 0, SEEK_END) == 0) size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    logWarning(tag, "could not read %s: %s", path.c_str(), std::strerror(errno));
    std::fclose(file);
    return std::nullopt;
  }
  text::BigText text = text::BigText::uninitialised(static_cast<std::size_t>(size));
  if (text.failed()) {
    logWarning(tag, "no memory to read %s (%ld bytes)", path.c_str(), size);
    std::fclose(file);
    return std::nullopt;
  }
  text.truncate(std::fread(text.data(), 1, text.size(), file));
  std::fclose(file);
  return text;
}

// The parser unescapes into a scratch block the size of the text, so that goes in PSRAM too.
struct Object {
  ble::JsonObjectReader reader;
  text::BigText scratch;
  bool read(std::string_view json) {
    scratch = text::BigText::uninitialised(json.size() + 16);
    if (scratch.failed()) return false;
    return reader.read(json, std::span<char>(scratch.data(), scratch.size()));
  }
  const ble::JsonMember* find(std::string_view key) const { return reader.find(key); }
};

std::optional<StoredReply> parseStoredReply(std::string_view json) {
  Object object;
  if (!object.read(json)) return std::nullopt;
  const ble::JsonMember* component = object.find("component");
  const ble::JsonMember* payloadHex = object.find("payloadHex");
  const ble::JsonMember* failure = object.find("failure");
  const auto stringOrNull = [](const ble::JsonMember* member) {
    return member != nullptr && (member->type == ble::JsonType::kString || member->type == ble::JsonType::kNull);
  };
  if (component == nullptr || component->type != ble::JsonType::kNumber || !stringOrNull(payloadHex) ||
      !stringOrNull(failure)) {
    return std::nullopt;
  }
  StoredReply reply{static_cast<int>(component->number), std::nullopt, std::nullopt};
  if (payloadHex->type == ble::JsonType::kString) reply.payloadHex = std::string(payloadHex->text);
  if (failure->type == ble::JsonType::kString) reply.failure = std::string(failure->text);
  return reply;
}

std::optional<std::vector<StoredReply>> parseReplies(const ble::JsonMember* member) {
  if (member == nullptr || member->type != ble::JsonType::kArray) return std::nullopt;
  std::vector<std::string_view> elements;
  if (!ble::jsonArrayElements(member->text, elements)) return std::nullopt;
  // Sized once: a freeze-frame reading can list 63 components, and doubling would hold two lists.
  std::vector<StoredReply> replies;
  replies.reserve(elements.size());
  for (const std::string_view element : elements) {
    std::optional<StoredReply> reply = parseStoredReply(element);
    if (!reply) return std::nullopt;
    replies.push_back(std::move(*reply));
  }
  return replies;
}

void writeRepliesJson(text::TextBuffer& out, const std::vector<StoredReply>& replies) {
  out.append("[");
  for (std::size_t at = 0; at < replies.size(); at++) {
    if (at > 0) out.append(",");
    writeStoredReplyJson(out, replies[at]);
  }
  out.append("]");
}

obd::FreezeFrameResponse decodeStoredReply(const StoredReply& reply) {
  if (reply.failure) return obd::unrecognisedResponse(*reply.failure, reply.payloadHex.value_or(""));
  if (!reply.payloadHex) return obd::unrecognisedResponse("no reply", "");
  const std::optional<std::vector<std::uint8_t>> bytes = bytesFromHex(*reply.payloadHex);
  if (!bytes) return obd::unrecognisedResponse("stored payload is not hex bytes", *reply.payloadHex);
  return obd::decodeFreezeFrameResponse(*bytes, reply.component);
}

std::string isoStamp(double wallClockMs) { return isoStampForFile(wallClockMs); }

bool listParsedCleanly(const FreezeFrameListReport& list) { return !describeListDamage(list).has_value(); }

std::optional<FreezeFrameListReport> parseListReport(std::string_view json) {
  Object object;
  if (!object.read(json)) return std::nullopt;
  FreezeFrameListReport list;
  const auto number = [&](const char* key) {
    const ble::JsonMember* member = object.find(key);
    return member != nullptr && member->type == ble::JsonType::kNumber ? static_cast<int>(member->number) : 0;
  };
  list.declaredCount = number("declaredCount");
  list.parsed = number("parsed");
  list.padding = number("padding");
  list.outOfRange = number("outOfRange");
  list.duplicate = number("duplicate");
  const ble::JsonMember* truncated = object.find("truncated");
  list.truncated = truncated != nullptr && truncated->text == "true";
  const ble::JsonMember* trailing = object.find("trailingHex");
  if (trailing != nullptr && trailing->type == ble::JsonType::kString) list.trailingHex = std::string(trailing->text);
  return list;
}

void writeFreezeFrameReadJson(text::TextBuffer& out, const StoredFreezeFrameRead& read) {
  out.append("{\"readAt\":");
  out.appendNumber(read.readAt);
  out.append(",\"source\":");
  out.appendJsonString(read.source);
  out.append(",\"completion\":");
  out.appendJsonString(read.completion);
  out.append(",\"components\":[");
  for (std::size_t at = 0; at < read.components.size(); at++) {
    if (at > 0) out.append(",");
    out.appendInteger(read.components[at]);
  }
  out.append("],\"replies\":");
  writeRepliesJson(out, read.replies);
  out.append(",\"list\":");
  if (read.list) {
    writeListReportJson(out, *read.list);
  } else {
    out.append("null");
  }
  out.append("}");
}

}  // namespace

bool isValidFaultKey(double component, double symptom) {
  return std::floor(component) == component && component >= 1 && component <= 63 && std::floor(symptom) == symptom &&
         symptom >= 0 && symptom <= 15;
}

void writeExpectedFaultsJson(text::TextBuffer& out, const ExpectedFaults& expected) {
  out.append("{\"entries\":[");
  for (std::size_t at = 0; at < expected.entries.size(); at++) {
    if (at > 0) out.append(",");
    out.append("{\"component\":");
    out.appendInteger(expected.entries[at].component);
    out.append(",\"symptom\":");
    out.appendInteger(expected.entries[at].symptom);
    out.append("}");
  }
  out.append("],\"updatedAt\":");
  if (expected.updatedAt) {
    out.appendNumber(*expected.updatedAt);
  } else {
    out.append("null");
  }
  out.append("}");
}

bool ReadingStore::makeDirectory() {
  if (::mkdir(directory_.c_str(), 0775) == 0 || errno == EEXIST) return true;
  logWarning("vcu-sweep", "could not create %s: %s", directory_.c_str(), std::strerror(errno));
  return false;
}

bool ReadingStore::writeFile(const std::string& target, std::string_view text) {
  // Written aside, then moved over the old one — FAT can't rename onto a file, so it goes first.
  const std::string temporary = target + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) return false;
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fflush(file) == 0;
  std::fclose(file);
  if (!written) return false;
  std::remove(target.c_str());
  return std::rename(temporary.c_str(), target.c_str()) == 0;
}

// --- Lifetime -------------------------------------------------------------------------------

std::optional<StoredLifetimeRead> ReadingStore::loadLifetimeRead() {
  const std::string file = path("lifetime.json");
  const std::optional<text::BigText> text = readWholeFile(file, "lifetime");
  if (!text) return std::nullopt;
  Object object;
  if (!object.read(text->view())) {
    logWarning("lifetime", "%s is not valid JSON:", file.c_str());
    return std::nullopt;
  }
  const ble::JsonMember* readAt = object.find("readAt");
  const ble::JsonMember* source = object.find("source");
  std::optional<std::vector<StoredReply>> replies = parseReplies(object.find("replies"));
  if (readAt == nullptr || readAt->type != ble::JsonType::kNumber || !replies) {
    logWarning("lifetime", "%s parsed but is not a stored reading", file.c_str());
    return std::nullopt;
  }
  return StoredLifetimeRead{readAt->number,
                            source != nullptr && source->type == ble::JsonType::kString ? std::string(source->text) : "",
                            std::move(*replies)};
}

ReadingStore::Written ReadingStore::writeLifetimeRead(const StoredLifetimeRead& read) {
  const int answered = answeredCount(read.replies);
  const std::string latest = path("lifetime.json");
  makeDirectory();
  const text::BigText serialised = build([&](text::TextBuffer& out) {
    out.append("{\"readAt\":");
    out.appendNumber(read.readAt);
    out.append(",\"source\":");
    out.appendJsonString(read.source);
    out.append(",\"replies\":");
    writeRepliesJson(out, read.replies);
    out.append("}\n");
  });
  if (serialised.failed()) {
    const std::string reason = "no memory to store the reading, so " + latest + " is left as it was";
    logWarning("lifetime", "⚠️  %s", reason.c_str());
    return {false, reason, answered};
  }
  writeFile(path("lifetime-" + isoStamp(read.readAt) + ".json"), serialised.view());
  const std::optional<StoredLifetimeRead> previous = loadLifetimeRead();
  const int previousAnswered = previous ? answeredCount(previous->replies) : 0;
  if (answered == 0) {
    const std::string reason = "nothing answered, so " + latest + " is left as it was — " +
                               std::to_string(previousAnswered) + " stored replies stand";
    logWarning("lifetime", "⚠️  %s", reason.c_str());
    return {false, reason, answered};
  }
  if (answered < previousAnswered) {
    const std::string reason = std::to_string(answered) + " replies would replace " + std::to_string(previousAnswered) +
                               " — KEPT the previous lifetime.json";
    logWarning("lifetime", "⚠️  %s", reason.c_str());
    return {false, reason, answered};
  }
  writeFile(latest, serialised.view());
  const std::string reason = "stored " + std::to_string(answered) + "/" + std::to_string(read.replies.size()) +
                             " replies from " + read.source + " in " + latest;
  logInfo("lifetime", "%s", reason.c_str());
  return {true, reason, answered};
}

text::BigText ReadingStore::lifetimeStatsBody() {
  const std::optional<StoredLifetimeRead> stored = loadLifetimeRead();
  return build([&](text::TextBuffer& out) {
    out.append("{\"reading\":");
    if (stored) {
      std::vector<obd::ComponentResponse> responses;
      responses.reserve(stored->replies.size());
      for (const StoredReply& reply : stored->replies) responses.push_back({reply.component, decodeStoredReply(reply)});
      out.append("{\"statistics\":");
      obd::writeLifetimeStatisticsJson(out, obd::summariseLifetimeStatistics(stored->readAt, responses));
      out.append(",\"source\":");
      out.appendJsonString(stored->source);
      out.append("}");
    } else {
      out.append("null");
    }
    out.append(",\"howToRead\":");
    out.appendJsonString(kLifetimeHowToRead);
    out.append(",\"howToReadWithServiceStopped\":");
    out.appendJsonString(kLifetimeHowToReadWithServiceStopped);
    out.append("}");
  });
}

// --- Freeze frames --------------------------------------------------------------------------

namespace {

// freeze-frames.json, read and checked, with its replies left as text to be read one at a time: a
// reading can list 63 components, and holding all their replies at once would be several KB of
// internal RAM. The file's text and the parser's scratch are in PSRAM.
struct FreezeFrameFile {
  text::BigText text;
  Object object;
  double readAt = 0;
  std::vector<std::string_view> replies;  // each a StoredReply's JSON
  std::vector<int> components;

  std::string member(const char* key) const {
    const ble::JsonMember* found = object.find(key);
    return found != nullptr && found->type == ble::JsonType::kString ? std::string(found->text) : std::string();
  }
  std::optional<FreezeFrameListReport> list() const {
    const ble::JsonMember* found = object.find("list");
    if (found == nullptr || found->type != ble::JsonType::kObject) return std::nullopt;
    return parseListReport(found->text);
  }
};

// Not there, unreadable, or not a stored reading (logged): nullopt.
std::unique_ptr<FreezeFrameFile> openFreezeFrameFile(const std::string& file) {
  std::optional<text::BigText> text = readWholeFile(file, "freeze-frame");
  if (!text) return nullptr;
  auto opened = std::make_unique<FreezeFrameFile>();
  opened->text = std::move(*text);
  if (!opened->object.read(opened->text.view())) {
    logWarning("freeze-frame", "%s is not valid JSON:", file.c_str());
    return nullptr;
  }
  const ble::JsonMember* readAt = opened->object.find("readAt");
  const ble::JsonMember* replies = opened->object.find("replies");
  bool valid = readAt != nullptr && readAt->type == ble::JsonType::kNumber && replies != nullptr &&
               replies->type == ble::JsonType::kArray && ble::jsonArrayElements(replies->text, opened->replies);
  for (std::size_t at = 0; valid && at < opened->replies.size(); at++) {
    valid = parseStoredReply(opened->replies[at]).has_value();
  }
  const ble::JsonMember* components = opened->object.find("components");
  std::vector<std::string_view> componentElements;
  valid = valid && components != nullptr && components->type == ble::JsonType::kArray &&
          ble::jsonArrayElements(components->text, componentElements);
  opened->components.reserve(componentElements.size());
  for (const std::string_view element : componentElements) {
    char* end = nullptr;
    const std::string copy(element);
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) valid = false;
    opened->components.push_back(static_cast<int>(value));
  }
  if (!valid) {
    logWarning("freeze-frame", "%s parsed but is not a stored reading", file.c_str());
    return nullptr;
  }
  opened->readAt = readAt->number;
  return opened;
}

}  // namespace

std::optional<StoredFreezeFrameRead> ReadingStore::loadFreezeFrameRead() {
  const std::unique_ptr<FreezeFrameFile> opened = openFreezeFrameFile(path("freeze-frames.json"));
  if (!opened) return std::nullopt;
  std::vector<StoredReply> replies;
  replies.reserve(opened->replies.size());
  for (const std::string_view element : opened->replies) {
    if (std::optional<StoredReply> reply = parseStoredReply(element)) replies.push_back(std::move(*reply));
  }
  return StoredFreezeFrameRead{opened->readAt,           opened->member("source"), opened->member("completion"),
                               opened->components,       std::move(replies),       opened->list()};
}

ReadingStore::Written ReadingStore::writeFreezeFrameRead(const StoredFreezeFrameRead& read) {
  const int answered = answeredCount(read.replies);
  const std::string latest = path("freeze-frames.json");
  makeDirectory();
  const text::BigText serialised = build([&](text::TextBuffer& out) {
    writeFreezeFrameReadJson(out, read);
    out.append("\n");
  });
  const auto refuse = [&](const std::string& reason) {
    logWarning("freeze-frame", "⚠️  %s", reason.c_str());
    return Written{false, reason, answered};
  };
  if (serialised.failed()) return refuse("no memory to store the reading, so freeze-frames.json is left as it was");
  writeFile(path("freeze-frames-" + isoStamp(read.readAt) + ".json"), serialised.view());
  if (!read.list) return refuse("the 0x18 list was not read, so freeze-frames.json is left as it was");
  const bool emptyList = read.components.empty();
  if (answered == 0 && !(emptyList && listParsedCleanly(*read.list))) {
    return refuse(emptyList ? "the 0x18 list parsed to nothing usable (" + describeListDamage(*read.list).value_or("null") +
                                  ") — freeze-frames.json is left as it was"
                            : std::string("nothing answered, so freeze-frames.json is left as it was"));
  }
  // Components the stored reading had, and this one lost though the bike still lists them.
  std::vector<int> degraded;
  if (const std::optional<StoredFreezeFrameRead> previous = loadFreezeFrameRead()) {
    std::set<int> answeredNow;
    for (const StoredReply& reply : read.replies) {
      if (!reply.failure) answeredNow.insert(reply.component);
    }
    const std::set<int> stillListed(read.components.begin(), read.components.end());
    const bool believable = listParsedCleanly(*read.list);
    for (const StoredReply& reply : previous->replies) {
      if (reply.failure || answeredNow.count(reply.component) > 0) continue;
      if (!believable || stillListed.count(reply.component) > 0) degraded.push_back(reply.component);
    }
  }
  if (!degraded.empty()) {
    std::string list;
    for (const int component : degraded) list += (list.empty() ? "" : ", ") + std::to_string(component);
    const std::optional<std::string> damage = describeListDamage(*read.list);
    const std::string why = damage ? "the 0x18 list " + *damage : std::string("the bike still lists them");
    return refuse("component(s) " + list + " answered in the stored reading and did not now, and " + why +
                  " — KEPT the previous freeze-frames.json");
  }
  writeFile(latest, serialised.view());
  const std::string reason = "stored " + std::to_string(answered) + "/" + std::to_string(read.replies.size()) +
                             " replies from " + read.source + " in " + latest;
  logInfo("freeze-frame", "%s", reason.c_str());
  return {true, reason, answered};
}

text::BigText ReadingStore::freezeFramesBody() {
  const std::unique_ptr<FreezeFrameFile> stored = openFreezeFrameFile(path("freeze-frames.json"));
  const std::optional<FreezeFrameListReport> list = stored ? stored->list() : std::nullopt;
  return build([&](text::TextBuffer& out) {
    out.append("{\"reading\":");
    if (stored) {
      out.append("{\"readAt\":");
      out.appendNumber(stored->readAt);
      out.append(",\"source\":");
      out.appendJsonString(stored->member("source"));
      out.append(",\"completion\":");
      out.appendJsonString(stored->member("completion"));
      out.append(",\"components\":[");
      for (std::size_t at = 0; at < stored->components.size(); at++) {
        if (at > 0) out.append(",");
        out.appendInteger(stored->components[at]);
      }
      out.append("],\"list\":");
      if (list) {
        writeListReportJson(out, *list);
      } else {
        out.append("null");
      }
      out.append(",\"records\":[");
      // One reply at a time, decoded and written, then let go.
      for (std::size_t at = 0; at < stored->replies.size(); at++) {
        const std::optional<StoredReply> parsed = parseStoredReply(stored->replies[at]);
        if (!parsed) continue;  // checked when the file was opened
        const StoredReply& reply = *parsed;
        if (at > 0) out.append(",");
        const std::optional<std::vector<std::uint8_t>> bytes =
            reply.payloadHex ? bytesFromHex(*reply.payloadHex) : std::nullopt;
        out.append("{\"component\":");
        out.appendInteger(reply.component);
        out.append(",\"response\":");
        std::optional<std::string> failure = reply.failure;
        if (!bytes) {
          const std::string reason = !reply.payloadHex ? reply.failure.value_or("no reply")
                                                       : std::string("stored payload is not hex bytes");
          obd::writeFreezeFrameResponseJson(out, obd::unrecognisedResponse(reason, reply.payloadHex.value_or("")));
          if (!failure) failure = reason;
        } else {
          obd::writeFreezeFrameResponseJson(out, obd::decodeFreezeFrameResponse(*bytes, reply.component));
        }
        out.append(",\"failure\":");
        if (failure) {
          out.appendJsonString(*failure);
        } else {
          out.append("null");
        }
        out.append("}");
      }
      out.append("]}");
    } else {
      out.append("null");
    }
    out.append(",\"howToRead\":");
    out.appendJsonString(kFreezeFramesHowToRead);
    out.append("}");
  });
}

// --- Expected faults ------------------------------------------------------------------------

ExpectedFaults ReadingStore::loadExpectedFaults() {
  const std::string file = path("expected-faults.json");
  const std::optional<text::BigText> text = readWholeFile(file, "expected-faults");
  if (!text) return {};
  Object object;
  if (!object.read(text->view())) {
    logWarning("expected-faults", "%s is not valid JSON:", file.c_str());
    return {};
  }
  const ble::JsonMember* entries = object.find("entries");
  std::vector<std::string_view> elements;
  ExpectedFaults expected;
  bool valid = entries != nullptr && entries->type == ble::JsonType::kArray && ble::jsonArrayElements(entries->text, elements);
  for (const std::string_view element : elements) {
    Object entry;
    const ble::JsonMember* component = nullptr;
    const ble::JsonMember* symptom = nullptr;
    if (!entry.read(element) || (component = entry.find("component")) == nullptr ||
        (symptom = entry.find("symptom")) == nullptr || component->type != ble::JsonType::kNumber ||
        symptom->type != ble::JsonType::kNumber || !isValidFaultKey(component->number, symptom->number)) {
      valid = false;
      break;
    }
    expected.entries.push_back({static_cast<int>(component->number), static_cast<int>(symptom->number)});
  }
  if (!valid) {
    logWarning("expected-faults", "%s parsed but is not a list of (component, symptom) pairs", file.c_str());
    return {};
  }
  if (const ble::JsonMember* updatedAt = object.find("updatedAt"); updatedAt != nullptr && updatedAt->type == ble::JsonType::kNumber) {
    expected.updatedAt = updatedAt->number;
  }
  return expected;
}

ExpectedFaults ReadingStore::setExpectedFault(ExpectedFault fault, bool expected, double wallClockMs) {
  const ExpectedFaults current = loadExpectedFaults();
  std::vector<ExpectedFault> without;
  for (const ExpectedFault& entry : current.entries) {
    if (entry.component != fault.component || entry.symptom != fault.symptom) without.push_back(entry);
  }
  if (!expected && without.size() == current.entries.size()) return current;
  if (expected && without.size() != current.entries.size()) return current;
  ExpectedFaults next;
  next.entries = without;
  if (expected) {
    next.entries.push_back(fault);
    std::stable_sort(next.entries.begin(), next.entries.end(), [](const ExpectedFault& left, const ExpectedFault& right) {
      return left.component != right.component ? left.component < right.component : left.symptom < right.symptom;
    });
  }
  next.updatedAt = wallClockMs;
  makeDirectory();
  const text::BigText serialised = build([&](text::TextBuffer& out) {
    writeExpectedFaultsJson(out, next);
    out.append("\n");
  });
  if (serialised.failed() || !writeFile(path("expected-faults.json"), serialised.view())) {
    logWarning("expected-faults", "could not save %s", path("expected-faults.json").c_str());
  }
  logInfo("expected-faults", "%d/%d %s — %zu on the list", fault.component, fault.symptom,
          expected ? "marked expected" : "unmarked", next.entries.size());
  return next;
}

}  // namespace emcool::vcu
