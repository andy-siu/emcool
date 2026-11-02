#include "emcool/vcu/service_api.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "emcool/vcu/bodies.hpp"

namespace emcool::vcu {

const char* const kServiceModeDisabled = "service mode is switched off on this bike (SERVICE_MODE_ENABLED=0)";

namespace {

template <typename Write>
std::string build(Write write) {
  text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
  write(measure);
  std::string text(measure.total(), '\0');
  text::TextBuffer out(text.data(), text.size());
  write(out);
  text.resize(out.length());
  return text;
}

void writeOptionalString(text::TextBuffer& out, const std::optional<std::string>& value) {
  if (value) {
    out.appendJsonString(*value);
  } else {
    out.append("null");
  }
}

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

// URLSearchParams.get(): the first value for `key`, decoded; nullopt if absent.
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

// URLSearchParams.getAll(): every value for `key`, in order, decoded.
std::vector<std::string> queryValues(std::string_view query, std::string_view key) {
  std::vector<std::string> values;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    const std::size_t equals = pair.find('=');
    if (urlDecode(pair.substr(0, equals)) == key) {
      values.push_back(equals == std::string_view::npos ? std::string() : urlDecode(pair.substr(equals + 1)));
    }
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return values;
}

// Number(text) on a query value, null for a missing one: NaN as nullopt.
std::optional<double> numberFromQuery(const std::optional<std::string>& raw) {
  if (!raw) return 0.0;  // Number(null)
  std::string_view text = *raw;
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (text.empty()) return 0.0;
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size()) return std::nullopt;
  for (const char character : copy) {
    if (std::isalpha(static_cast<unsigned char>(character)) && character != 'e' && character != 'E') return std::nullopt;
  }
  return value;
}

}  // namespace

std::string ServiceApi::readBody(const std::optional<std::string>& message) {
  return build([&](text::TextBuffer& out) { writeReadResponseJson(out, runner_, store_, enabled_, message); });
}

async::Task<ServiceAnswer> ServiceApi::answer(ServiceRequest request) {
  using Kind = ServiceRequest::Kind;
  const auto probeBody = [&](const VcuProbeReading* reading, const std::optional<std::string>& message) {
    return build([&](text::TextBuffer& out) { writeProbeResponseJson(out, reading, message, runner_.gate()); });
  };
  const auto lifetimeBody = [&](std::optional<std::string> measurement, std::optional<int> answered,
                                std::optional<std::string> message) {
    return build([&](text::TextBuffer& out) {
      out.append("{\"measurement\":");
      writeOptionalString(out, measurement);
      out.append(",\"answered\":");
      if (answered) {
        out.appendInteger(*answered);
      } else {
        out.append("null");
      }
      out.append(",\"message\":");
      writeOptionalString(out, message);
      out.append(",\"gate\":");
      writeVerdictJson(out, runner_.gate());
      out.append("}");
    });
  };
  const auto freezeBody = [&](std::optional<std::string> summary, std::optional<std::string> message) {
    return build([&](text::TextBuffer& out) {
      out.append("{\"summary\":");
      writeOptionalString(out, summary);
      out.append(",\"message\":");
      writeOptionalString(out, message);
      out.append(",\"gate\":");
      writeVerdictJson(out, runner_.gate());
      out.append("}");
    });
  };
  const auto expectedBody = [&](const ExpectedFaults& expected, std::optional<std::string> message) {
    return build([&](text::TextBuffer& out) {
      out.append("{\"expected\":");
      writeExpectedFaultsJson(out, expected);
      out.append(",\"message\":");
      writeOptionalString(out, message);
      out.append("}");
    });
  };

  switch (request.kind) {
    case Kind::kReadState:
      co_return ServiceAnswer{200, readBody(std::nullopt)};
    case Kind::kReadStart: {
      if (!enabled_) co_return ServiceAnswer{403, readBody(std::string(kServiceModeDisabled))};
      const VcuReadRunner::Started started = runner_.start();
      co_return ServiceAnswer{started.started ? 202 : 409, readBody(started.reason)};
    }
    case Kind::kReadCancel: {
      const bool cancelled = runner_.cancel();
      co_return ServiceAnswer{cancelled ? 202 : 409,
                              readBody(cancelled ? std::nullopt : std::optional<std::string>("no sweep is running"))};
    }
    case Kind::kProbe: {
      if (!enabled_) co_return ServiceAnswer{403, probeBody(nullptr, std::string(kServiceModeDisabled))};
      const ProbeParse parsed = parseProbeRequest(request.target, request.bank, request.index);
      if (!parsed.request) co_return ServiceAnswer{400, probeBody(nullptr, parsed.reason)};
      const ProbeOutcome outcome = co_await runner_.probe(*parsed.request);
      if (!outcome.reading) co_return ServiceAnswer{409, probeBody(nullptr, outcome.reason)};
      co_return ServiceAnswer{200, probeBody(&*outcome.reading, std::nullopt)};
    }
    case Kind::kLifetimeRead: {
      if (!enabled_) co_return ServiceAnswer{403, lifetimeBody(std::nullopt, std::nullopt, std::string(kServiceModeDisabled))};
      const LifetimeOutcome outcome = co_await runner_.readLifetimeStatistics();
      if (!outcome.result) co_return ServiceAnswer{409, lifetimeBody(std::nullopt, std::nullopt, outcome.reason)};
      const ReadingStore::Written stored =
          readings_.writeLifetimeRead({wallClockMs_(), "service", outcome.result->replies});
      co_return ServiceAnswer{200, lifetimeBody(describeMeasurement(outcome.result->flowControl), stored.answered,
                                                stored.stored ? describeFailures(outcome.result->replies)
                                                              : std::optional<std::string>(stored.reason))};
    }
    case Kind::kFreezeFrameRead: {
      if (!enabled_) co_return ServiceAnswer{403, freezeBody(std::nullopt, std::string(kServiceModeDisabled))};
      const FreezeFrameOutcome outcome = co_await runner_.readFreezeFrames();
      if (!outcome.result) co_return ServiceAnswer{409, freezeBody(std::nullopt, outcome.reason)};
      const FreezeFrameReadResult& result = *outcome.result;
      const ReadingStore::Written stored = readings_.writeFreezeFrameRead(
          {wallClockMs_(), "service", result.completion, result.components, result.replies, result.list});
      co_return ServiceAnswer{200, freezeBody(describeFreezeFrameRead(result),
                                              stored.stored ? describeFailures(result.replies)
                                                            : std::optional<std::string>(stored.reason))};
    }
    case Kind::kLifetimeStats:
      co_return ServiceAnswer{200, readings_.lifetimeStatsBody()};
    case Kind::kFreezeFrames:
      co_return ServiceAnswer{200, readings_.freezeFramesBody()};
    case Kind::kExpectedFaults:
      co_return ServiceAnswer{200, expectedBody(readings_.loadExpectedFaults(), std::nullopt)};
    case Kind::kSetExpectedFault: {
      const std::optional<double> component = numberFromQuery(request.component);
      const std::optional<double> symptom = numberFromQuery(request.symptom);
      if (!component || !symptom || !isValidFaultKey(*component, *symptom)) {
        co_return ServiceAnswer{400, expectedBody(readings_.loadExpectedFaults(),
                                                  "component and symptom must name a code this bike could report — got " +
                                                      request.component.value_or("null") + "/" +
                                                      request.symptom.value_or("null"))};
      }
      if (request.expected != "0" && request.expected != "1") {
        const std::string raw = request.expected ? build([&](text::TextBuffer& out) { out.appendJsonString(*request.expected); })
                                                 : std::string("null");
        co_return ServiceAnswer{400, expectedBody(readings_.loadExpectedFaults(), "expected must be 0 or 1 — got " + raw)};
      }
      const ExpectedFaults next = readings_.setExpectedFault(
          {static_cast<int>(*component), static_cast<int>(*symptom)}, request.expected == "1", wallClockMs_());
      co_return ServiceAnswer{200, expectedBody(next, std::nullopt)};
    }
    case Kind::kWriteStatus:
    case Kind::kWritePerform: {
      if (writeRunner_ == nullptr) co_return ServiceAnswer{503, readBody("writing is not available")};
      const std::string query = request.query;
      const auto get = [&query](std::string_view key) -> std::optional<std::string> {
        return queryValue(query, key);
      };
      const auto getAll = [&query](std::string_view key) { return queryValues(query, key); };
      const WriteStatusRequest statusReq = parseStatusRequest(get);
      const auto body = [&](const ServiceWriteResult* result, const std::optional<std::string>& message) {
        return build([&](text::TextBuffer& out) { writeRunner_->writeResponseJson(out, statusReq, result, message); });
      };
      if (request.kind == Kind::kWriteStatus) {
        co_return ServiceAnswer{200, body(nullptr, std::nullopt)};
      }
      const WriteRequestParse parsed = parseWriteRequest(get, getAll, wallClockMs_());
      if (!parsed.ok) co_return ServiceAnswer{400, body(nullptr, parsed.reason)};
      const ServiceWriteAnswer answer = co_await writeRunner_->perform(parsed.request);
      if (!answer.ok) co_return ServiceAnswer{409, body(nullptr, answer.reason)};
      co_return ServiceAnswer{200, body(&answer.result, std::nullopt)};
    }
    case Kind::kChargeAutoStatus:
    case Kind::kChargeAutoSet:
      // Handled by the diagnostics task directly (the controller is its own, not a runner here).
      co_return ServiceAnswer{500, "{}"};
  }
  co_return ServiceAnswer{500, "{}"};
}

}  // namespace emcool::vcu
