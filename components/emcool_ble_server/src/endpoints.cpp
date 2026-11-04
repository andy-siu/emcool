#include "emcool/ble_server/endpoints.hpp"

#include <algorithm>
#include <cstring>

#include "emcool/obd/static_bodies.hpp"
#include "emcool/text/text_buffer.hpp"

namespace emcool::ble_server {

namespace {

using text::TextBuffer;

constexpr const char* kJson = "application/json; charset=utf-8";
constexpr const char* kText = "text/plain; charset=utf-8";
constexpr const char* kOctets = "application/octet-stream";
constexpr std::size_t kMaxGroups = 32;

struct GroupCount {
  const char* group;
  std::uint32_t live;
  std::uint32_t total;
  bool onDemandOnly;
};

// A group whose every signal is written only on request reads 0 live at rest, and anything
// filtering on live == 0 to find a dead source would fire on it forever — so it is left out
// of the summary.
std::size_t collectGroups(GroupCount (&groups)[kMaxGroups]) {
  std::size_t count = 0;
  for (const signals::SignalDef& signal : signals::signalRegistry()) {
    std::size_t index = 0;
    while (index < count && std::strcmp(groups[index].group, signal.group) != 0) index++;
    if (index == count) {
      if (count == kMaxGroups) continue;
      groups[count++] = {signal.group, 0, 0, true};
    }
    groups[index].total++;
    groups[index].onDemandOnly = groups[index].onDemandOnly && signal.onDemand;
  }
  return count;
}

}  // namespace

std::optional<HttpResponse> DashboardEndpoints::begin(const HttpRequest& request, std::int64_t nowMonotonicMs) {
  const std::size_t mark = request.path.find('?');
  const std::string_view route = request.path.substr(0, mark);
  const std::string_view query = mark == std::string_view::npos ? std::string_view() : request.path.substr(mark + 1);
  endDownload();
  waiting_ = Waiting::kNothing;
  parts_.reset();
  const std::string_view method = request.method;
  if (route == "/dl") {
    if (method != "GET") return fromText(405, kText, "use GET to download the ride log\n");
    if (download_ == nullptr) return fromText(503, kText, "emcool: the ride log is off (no microSD pins configured)\n");
    return beginDownload(query);
  }
  if (method == "GET") {
    if (route == "/status") return status(nowMonotonicMs);
    if (route == "/dtc-table" || route == "/fault-infokeys") {
      source_ = Source::kStatic;
      static_ = route == "/dtc-table" ? obd::dtcTableBody() : obd::faultInfokeysBody();
      length_ = static_.size();
      return HttpResponse{200, kJson, length_};
    }
    if (route == "/stored-dtcs") {
      source_ = Source::kStoredCodes;
      codes_ = troubleCodes_.troubleCodes();
      codesAtMs_ = nowMonotonicMs;
      TextBuffer measure = TextBuffer::window(buffer_.data(), 0, 0);
      codes_.writeJson(measure, codesAtMs_);
      length_ = measure.total();
      return HttpResponse{200, kJson, length_};
    }
  }
  if (service_ != nullptr &&
      (route == "/vcu-read" || route == "/vcu-probe" || route == "/vcu-params" || route == "/vcu-backup" ||
       route == "/lifetime-read" || route == "/freeze-frame-read" || route == "/lifetime-stats" ||
       route == "/freeze-frames" || route == "/expected-faults" || route == "/vcu-write" ||
       route == "/charge-auto")) {
    return service(request, route, query);
  }
  TextBuffer text(buffer_.data(), buffer_.size());
  text.append("emcool: ");
  text.append(method);
  text.append(" ");
  text.append(route.substr(0, 64));
  text.append(" isn't available on the bike yet");
  return fromBuffer(404, kText, text.overflowed() ? 0 : text.length());
}

namespace {

int hexDigit(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

std::string urlDecode(std::string_view text) {
  std::string out;
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
}

// URLSearchParams.get(): the first value for `key`, decoded.
std::optional<std::string> queryValue(std::string_view query, std::string_view key) {
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    const std::size_t equals = pair.find('=');
    if (urlDecode(pair.substr(0, equals)) == key) {
      return equals == std::string_view::npos ? std::string() : urlDecode(pair.substr(equals + 1));
    }
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return std::nullopt;
}

}  // namespace

std::optional<HttpResponse> DashboardEndpoints::service(const HttpRequest& request, std::string_view route,
                                                        std::string_view query) {
  const std::string_view method = request.method;
  // The snapshots, straight off the card; they never touch the bus.
  if (route == "/vcu-params" || route == "/vcu-backup") {
    vcu::VcuStore* store = service_->store();
    if (store == nullptr) return fromText(503, kText, "emcool: no card for service mode's snapshots\n");
    if (route == "/vcu-params") {
      return fromParts(200, kJson, std::make_unique<vcu::ParamsBody>(*store, service_->displayDirectory()));
    }
    auto backup = std::make_unique<vcu::BackupBody>(*store, service_->displayDirectory());
    const int status = backup->status();
    const char* type = status == 200 ? "text/csv; charset=utf-8" : kText;
    return fromParts(status, type, std::move(backup));
  }
  const bool serviceMode = request.emcoolHeader == vcu::kServiceModeHeaderValue;
  const std::string needsServiceMode = "starting a read needs the x-emcool: service-mode header\n";
  ServiceRequest submitted{ServiceRequest::Kind::kReadState, {}, {}, {}, {}, {}, {}, {}};
  if (route == "/lifetime-read" || route == "/freeze-frame-read") {
    const bool lifetime = route == "/lifetime-read";
    if (method != "POST") {
      return fromText(405, kText,
                      lifetime ? "use POST to read the lifetime battery statistics; GET /lifetime-stats serves the last "
                                 "reading\n"
                               : "use POST to read the stored freeze frames; GET /freeze-frames serves the last reading\n");
    }
    if (!serviceMode) return fromText(403, kText, needsServiceMode);
    submitted.kind = lifetime ? ServiceRequest::Kind::kLifetimeRead : ServiceRequest::Kind::kFreezeFrameRead;
  } else if (route == "/lifetime-stats" || route == "/freeze-frames") {
    // Files, not the bus; served to any method.
    submitted.kind = route == "/lifetime-stats" ? ServiceRequest::Kind::kLifetimeStats : ServiceRequest::Kind::kFreezeFrames;
  } else if (route == "/expected-faults") {
    if (method == "GET") {
      submitted.kind = ServiceRequest::Kind::kExpectedFaults;
    } else if (method != "POST") {
      return fromText(405, kText, "use GET to read the expected-fault list and POST to change it\n");
    } else if (request.emcoolHeader != "expected-faults") {
      return fromText(403, kText, "changing the list needs the x-emcool: expected-faults header\n");
    } else {
      submitted.kind = ServiceRequest::Kind::kSetExpectedFault;
      submitted.component = queryValue(query, "component");
      submitted.symptom = queryValue(query, "symptom");
      submitted.expected = queryValue(query, "expected");
    }
  } else if (route == "/charge-auto") {
    // GET the controller's state (no header). POST ?mode=automatic|off, behind the charge-auto header.
    if (method == "GET") {
      submitted.kind = ServiceRequest::Kind::kChargeAutoStatus;
    } else if (method == "POST") {
      if (request.emcoolHeader != "charge-auto")
        return fromText(403, kText, "A POST here wants the x-emcool: charge-auto header\n");
      submitted.kind = ServiceRequest::Kind::kChargeAutoSet;
      submitted.target = queryValue(query, "mode");  // the mode, carried in `target`
    } else {
      return fromText(405, kText, "use GET to read the controller, POST ?mode=automatic|off to set it\n");
    }
  } else if (route == "/vcu-write") {
    // GET the allowlist, the gate, the clock verdict and the journal — touches nothing, not behind
    // the header. POST does exactly one thing to the bike, behind the SEPARATE service-write header.
    if (method == "GET") {
      submitted.kind = ServiceRequest::Kind::kWriteStatus;
    } else if (method == "POST") {
      if (request.emcoolHeader != "service-write")
        return fromText(403, kText, "writing needs the x-emcool: service-write header\n");
      submitted.kind = ServiceRequest::Kind::kWritePerform;
    } else {
      return fromText(405, kText, "use GET to see what may be written, POST to write one thing\n");
    }
    submitted.query = std::string(query);
  } else if (route == "/vcu-read") {
    if (method == "GET") {
      submitted.kind = ServiceRequest::Kind::kReadState;
    } else if (method == "POST") {
      if (!serviceMode) return fromText(403, kText, needsServiceMode);
      submitted.kind = ServiceRequest::Kind::kReadStart;
    } else if (method == "DELETE") {
      submitted.kind = ServiceRequest::Kind::kReadCancel;
    } else {
      return fromText(405, kText, "use GET to watch a parameter read, POST to start one, DELETE to stop it\n");
    }
  } else {
    if (method != "POST") return fromText(405, kText, "use POST to probe one identifier: /vcu-probe?target=A9&bank=2&index=5\n");
    if (!serviceMode) return fromText(403, kText, "probing needs the x-emcool: service-mode header\n");
    submitted.kind = ServiceRequest::Kind::kProbe;
    submitted.target = queryValue(query, "target");
    submitted.bank = queryValue(query, "bank");
    submitted.index = queryValue(query, "index");
  }
  if (!service_->submit(submitted)) return fromText(503, kText, "emcool: another service request is still being answered\n");
  waiting_ = Waiting::kService;
  return std::nullopt;
}

namespace {

// A whole number from 1 to `most`, or nullopt.
std::optional<std::uint64_t> positiveNumber(const std::optional<std::string>& text, std::uint64_t most) {
  if (!text || text->empty() || text->size() > 10) return std::nullopt;
  std::uint64_t number = 0;
  for (const char digit : *text) {
    if (digit < '0' || digit > '9') return std::nullopt;
    number = number * 10 + static_cast<std::uint64_t>(digit - '0');
  }
  if (number == 0 || number > most) return std::nullopt;
  return number;
}

}  // namespace

// GET /dl[?mb=N][&boot=N]. Planning reads the card, a step per poll, so the head comes later.
std::optional<HttpResponse> DashboardEndpoints::beginDownload(std::string_view query) {
  ride_log::DownloadSelection selection;
  if (const std::optional<std::string> mb = queryValue(query, "mb")) {
    const std::optional<std::uint64_t> megabytes = positiveNumber(mb, ride_log::kMaxDownloadBytes / ride_log::kMegabyte);
    if (!megabytes) {
      return fromText(400, kText,
                      "mb= wants a whole number of megabytes from 1 to " +
                          std::to_string(ride_log::kMaxDownloadBytes / ride_log::kMegabyte) + "\n");
    }
    selection.maxBytes = *megabytes * ride_log::kMegabyte;
  }
  if (const std::optional<std::string> boot = queryValue(query, "boot")) {
    const std::optional<std::uint64_t> number = positiveNumber(boot, 99999);
    if (!number) return fromText(400, kText, "boot= wants a boot number, as in RIDES/00012: 12\n");
    selection.boot = static_cast<std::uint32_t>(*number);
  }
  const ride_log::LogDownload::Outcome outcome = download_->start(selection);
  if (outcome == ride_log::LogDownload::Outcome::kMeasuring) {
    waiting_ = Waiting::kDownload;
    return std::nullopt;
  }
  return fromText(outcome == ride_log::LogDownload::Outcome::kNothing ? 404 : 503, kText,
                  "emcool: " + download_->problem() + "\n");
}

std::optional<HttpResponse> DashboardEndpoints::pollDownload() {
  for (int step = 0; step < kDownloadStepsPerPoll; step++) {
    switch (download_->measureNext()) {
      case ride_log::LogDownload::Outcome::kMeasuring:
        continue;
      case ride_log::LogDownload::Outcome::kReady: {
        waiting_ = Waiting::kNothing;
        source_ = Source::kDownload;
        length_ = static_cast<std::size_t>(download_->totalBytes());
        downloadName_ = download_->fileName();
        HttpResponse response{200, kOctets, length_};
        response.fileName = downloadName_.c_str();
        return response;
      }
      case ride_log::LogDownload::Outcome::kNothing:
        waiting_ = Waiting::kNothing;
        return fromText(404, kText, "emcool: " + download_->problem() + "\n");
      case ride_log::LogDownload::Outcome::kFailed:
        waiting_ = Waiting::kNothing;
        return fromText(503, kText, "emcool: " + download_->problem() + "\n");
    }
  }
  return std::nullopt;
}

// Lets go of the card's files; a no-op when no download is running.
void DashboardEndpoints::endDownload() {
  if (download_ != nullptr && (source_ == Source::kDownload || waiting_ == Waiting::kDownload)) download_->stop();
  if (source_ == Source::kDownload) {
    source_ = Source::kBuffer;
    length_ = 0;
  }
}

std::optional<HttpResponse> DashboardEndpoints::poll(std::int64_t) {
  if (waiting_ == Waiting::kDownload) return pollDownload();
  if (waiting_ != Waiting::kService) return timedOut();
  std::optional<ServiceAnswer> answer = service_->take();
  if (!answer) return std::nullopt;
  waiting_ = Waiting::kNothing;
  return fromText(answer->status, kJson, answer->body);
}

HttpResponse DashboardEndpoints::timedOut() {
  endDownload();
  waiting_ = Waiting::kNothing;
  return {504, kText, 0};
}

void DashboardEndpoints::abandon() {
  if (waiting_ == Waiting::kService && service_ != nullptr) service_->abandon();
  endDownload();
  waiting_ = Waiting::kNothing;
}

HttpResponse DashboardEndpoints::fromText(int status, const char* contentType, std::string_view text) {
  source_ = Source::kText;
  text_.assign(text);
  length_ = text_.size();
  return {status, contentType, length_};
}

HttpResponse DashboardEndpoints::fromParts(int status, const char* contentType, std::unique_ptr<vcu::PartsBody> parts) {
  // Measured first, for the length the head carries; then read again as the pieces go.
  length_ = 0;
  parts->rewind();
  while (const std::optional<std::string> part = parts->next()) length_ += part->size();
  parts->rewind();
  parts_ = std::move(parts);
  part_.clear();
  partOffset_ = 0;
  source_ = Source::kParts;
  return {status, contentType, length_};
}

std::span<const char> DashboardEndpoints::body(std::size_t offset, std::size_t maxBytes) {
  if (offset >= length_) return {};
  const std::size_t count = std::min(maxBytes, length_ - offset);
  switch (source_) {
    case Source::kBuffer:
      return {buffer_.data() + offset, count};
    case Source::kStatic:
      return {static_.data() + offset, count};
    case Source::kStoredCodes: {
      TextBuffer window = TextBuffer::window(buffer_.data(), std::min(count, buffer_.size()), offset);
      codes_.writeJson(window, codesAtMs_);
      return {buffer_.data(), window.length()};
    }
    case Source::kText:
      return {text_.data() + offset, count};
    case Source::kDownload: {
      if (download_ == nullptr) return {};
      const std::size_t got = download_->read(offset, std::span<char>(buffer_.data(), std::min(count, buffer_.size())));
      return {buffer_.data(), got};
    }
    case Source::kParts: {
      if (!parts_) return {};
      // Pieces are asked for in order; one asked for again from earlier starts over.
      if (offset < partOffset_) {
        parts_->rewind();
        part_.clear();
        partOffset_ = 0;
      }
      std::size_t filled = 0;
      const std::size_t wanted = std::min(count, buffer_.size());
      while (filled < wanted) {
        const std::size_t at = offset + filled;
        if (at >= partOffset_ + part_.size()) {
          partOffset_ += part_.size();
          std::optional<std::string> next = parts_->next();
          if (!next) break;
          part_ = std::move(*next);
          continue;
        }
        const std::size_t take = std::min(wanted - filled, partOffset_ + part_.size() - at);
        std::memcpy(buffer_.data() + filled, part_.data() + (at - partOffset_), take);
        filled += take;
      }
      return {buffer_.data(), filled};
    }
  }
  return {};
}

HttpResponse DashboardEndpoints::fromBuffer(int status, const char* contentType, std::size_t length) {
  source_ = Source::kBuffer;
  length_ = length;
  return {status, contentType, length};
}

HttpResponse DashboardEndpoints::status(std::int64_t nowMonotonicMs) {
  const std::span<char> body(buffer_);
  GroupCount groups[kMaxGroups];
  const std::size_t groupCount = collectGroups(groups);
  {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    const auto registry = signals::signalRegistry();
    for (std::size_t index = 0; index < registry.size(); index++) {
      if (!store_.seenAt(index) || nowMonotonicMs - store_.lastSeenMonotonicMsAt(index) >= kFreshMs) continue;
      for (std::size_t group = 0; group < groupCount; group++) {
        if (std::strcmp(groups[group].group, registry[index].group) == 0) {
          groups[group].live++;
          break;
        }
      }
    }
  }
  const StorageStatus storage = storage_.storage();

  TextBuffer text(body.data(), body.size());
  text.append("{\"uptimeSeconds\":");
  text.appendInteger(nowMonotonicMs / 1000);
  // Waypoints were dropped with location logging; the fields stay so the sheet's shape holds.
  text.append(",\"waypoints\":0,\"waypointsRefused\":0,\"waypointEvents\":[],\"log\":{\"files\":");
  text.appendUnsigned(storage.files);
  text.append(",\"bytes\":");
  text.appendUnsigned(storage.bytes);
  text.append(storage.logging ? ",\"enabled\":true}" : ",\"enabled\":false}");
  if (storage.freeBytes && storage.totalBytes) {
    text.append(",\"disk\":{\"freeBytes\":");
    text.appendUnsigned(*storage.freeBytes);
    text.append(",\"totalBytes\":");
    text.appendUnsigned(*storage.totalBytes);
    text.append("}");
  } else {
    text.append(",\"disk\":null");
  }
  text.append(",\"groups\":{");
  bool first = true;
  for (std::size_t group = 0; group < groupCount; group++) {
    if (groups[group].onDemandOnly) continue;
    if (!first) text.append(",");
    first = false;
    text.appendJsonString(groups[group].group);
    text.append(":[");
    text.appendUnsigned(groups[group].live);
    text.append(",");
    text.appendUnsigned(groups[group].total);
    text.append("]");
  }
  text.append("}}");
  if (text.overflowed()) {
    TextBuffer error(body.data(), body.size());
    error.append("emcool: the status did not fit its buffer");
    return fromBuffer(500, kText, error.length());
  }
  return fromBuffer(200, kJson, text.length());
}

}  // namespace emcool::ble_server
