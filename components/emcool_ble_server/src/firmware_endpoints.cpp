#include "emcool/ble_server/firmware_endpoints.hpp"

#include <optional>

#include "emcool/log.hpp"
#include "emcool/settings/dashboard_reply.hpp"

namespace emcool::ble_server {

namespace {

constexpr const char* kTag = "firmware";

using settings::jsonString;

FirmwareAnswer reply(int status, bool ok, std::string_view message, std::string extra = {}) {
  return {status, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":" + jsonString(message) + extra + "}"};
}

// The first value of `key` in a query: digits only, at most 10 of them.
std::optional<std::size_t> number(std::string_view query, std::string_view key) {
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    if (pair.size() > key.size() && pair.substr(0, key.size()) == key && pair[key.size()] == '=') {
      const std::string_view digits = pair.substr(key.size() + 1);
      if (digits.empty() || digits.size() > 10) return std::nullopt;
      std::uint64_t value = 0;
      for (const char digit : digits) {
        if (digit < '0' || digit > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(digit - '0');
      }
      return static_cast<std::size_t>(value);
    }
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return std::nullopt;
}

const char* stateName(update::Updater::State state) {
  switch (state) {
    case update::Updater::State::kIdle:
      return "idle";
    case update::Updater::State::kWriting:
      return "writing";
    case update::Updater::State::kDone:
      return "done";
    case update::Updater::State::kFailed:
      return "failed";
  }
  return "idle";
}

}  // namespace

bool FirmwareEndpoints::ours() const {
  return updater_.state() == update::Updater::State::kWriting && updater_.source() == kSource;
}

FirmwareAnswer FirmwareEndpoints::answer(std::string_view method, std::string_view route, std::string_view query,
                                         std::string_view header, std::string_view body) {
  if (route == "/firmware") {
    if (method != "GET") return reply(405, false, "use GET /firmware for what runs now; updates go to /firmware/begin");
    return status();
  }
  if (method != "POST") return reply(405, false, "use POST");
  if (header != kHeaderValue) {
    // The CSRF refusal shape, which tells the page to reload rather than retry.
    return {403,
            "{\"ok\":false,\"csrfRefusal\":true,\"message\":\"A firmware update needs the x-emcool: firmware "
            "header.\",\"reason\":\"A firmware update needs the x-emcool: firmware header.\"}"};
  }
  if (route == "/firmware/begin") return begin(query);
  if (route == "/firmware/chunk") return chunk(query, body);
  if (route == "/firmware/finish") return finish();
  if (route == "/firmware/cancel") return cancel();
  return reply(404, false, "the firmware endpoints are /firmware, /firmware/begin, /chunk, /finish and /cancel");
}

FirmwareAnswer FirmwareEndpoints::status() {
  std::string extra = ",\"report\":" + jsonString(host_.report());
  extra += ",\"state\":\"";
  extra += stateName(updater_.state());
  extra += "\",\"source\":" + jsonString(updater_.source());
  extra += ",\"received\":" + std::to_string(updater_.received());
  extra += ",\"total\":" + std::to_string(updater_.total());
  extra += ",\"problem\":" + jsonString(updater_.problem());
  extra += ",\"chunkBytes\":" + std::to_string(kChunkBytes);
  extra += ",\"chip\":" + std::to_string(updater_.chip());
  extra += ",\"chipName\":" + jsonString(update::chipName(updater_.chip()));
  return reply(200, true, "", extra);
}

FirmwareAnswer FirmwareEndpoints::begin(std::string_view query) {
  const std::optional<std::size_t> bytes = number(query, "bytes");
  if (!bytes) return reply(400, false, "bytes= wants the image's size in bytes");
  if (const std::string refusal = host_.refusal(); !refusal.empty()) {
    return reply(409, false, "not updating now: " + refusal);
  }
  std::string why;
  if (!updater_.start(*bytes, std::string(kSource), why)) return reply(409, false, "not updating: " + why);
  loggedTenth_ = 0;
  logWarning(kTag, "firmware update over Bluetooth: %zu bytes", *bytes);
  return reply(200, true, "send the image from offset 0", ",\"chunkBytes\":" + std::to_string(kChunkBytes));
}

FirmwareAnswer FirmwareEndpoints::chunk(std::string_view query, std::string_view body) {
  if (!ours()) return reply(409, false, "no Bluetooth update is under way: start one with /firmware/begin");
  const std::optional<std::size_t> offset = number(query, "offset");
  if (!offset) return reply(400, false, "offset= wants where this piece starts in the image");
  const std::string expected = ",\"expected\":" + std::to_string(updater_.received());
  if (*offset != updater_.received()) {
    return reply(409, false, "this piece starts at " + std::to_string(*offset) + ", but the bike has " +
                                 std::to_string(updater_.received()) + " bytes", expected);
  }
  if (body.empty() || body.size() > kChunkBytes) {
    return reply(400, false, "a piece is 1 to " + std::to_string(kChunkBytes) + " bytes", expected);
  }
  // The bike may have started moving since the last piece: a flash write drops CAN frames.
  if (const std::string refusal = host_.refusal(); !refusal.empty()) {
    updater_.cancel("stopped: " + refusal);
    return reply(409, false, "stopped: " + refusal + ". The running firmware is unchanged.");
  }
  if (!updater_.feed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()))) {
    return reply(400, false, "not updated: " + updater_.problem() + ". The running firmware is unchanged.");
  }
  const std::size_t tenth = updater_.total() == 0 ? 0 : updater_.received() * 10 / updater_.total();
  if (tenth > loggedTenth_) {
    loggedTenth_ = tenth;
    logInfo(kTag, "%zu0%% received", tenth);
  }
  return reply(200, true, "", ",\"received\":" + std::to_string(updater_.received()));
}

FirmwareAnswer FirmwareEndpoints::finish() {
  if (!ours()) return reply(409, false, "no Bluetooth update is under way: start one with /firmware/begin");
  if (!updater_.finish()) {
    return reply(400, false, "not updated: " + updater_.problem() + ". The running firmware is unchanged.");
  }
  return reply(200, true,
               "Written and checked: " + update::describe(*updater_.incoming()) +
                   ". It runs after a restart, on trial: let it run 30 s, since a crash or a restart before then goes "
                   "back to the firmware running now.",
               ",\"incoming\":" + jsonString(update::describe(*updater_.incoming())));
}

FirmwareAnswer FirmwareEndpoints::cancel() {
  if (!ours()) return reply(200, true, "no Bluetooth update was under way");
  updater_.cancel("cancelled from the dashboard");
  return reply(200, true, "cancelled; the running firmware is unchanged");
}

void FirmwareEndpoints::sessionEnded() {
  if (ours()) updater_.cancel("the phone went away mid-update");
}

}  // namespace emcool::ble_server
