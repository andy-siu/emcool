// The Bluetooth server on the host, for tools/check-ble.mts to drive with cool-eva's own
// phone-side protocol code. The radio is replaced by packets on stdin/stdout, each
// [type u8][length u32 BE][payload]:
//
//   in   C mtu(u16)  a phone connected        D  it disconnected
//        N on(u8)    notifications on/off      W  a GATT write: one frame
//        R           read the info value       P count(u32)  feed that many corpus frames
//        T ms(u32) budget(u32)   advance the clock in 10 ms steps, ticking the server and
//                    notifying at most `budget` frames per step (0xffffffff: all of them)
//        L           dump live state           Z  dump counters          Q  quit
//   out  I info JSON   F a notification frame   J a JSON dump   K done with the command
//
//   ble_server_host <secret> <golden decode corpus> [<mode 03 reply hex> [<RIDES dir>]]
//
// With a mode 03 reply, /stored-dtcs serves the stored list as if the bike had sent it. With a
// RIDES directory, /dl serves the ride log from it, as a mounted card with nothing being written.

#include <unistd.h>

#include <algorithm>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "emcool/ble/crypto.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/ble_server/server.hpp"
#include "emcool/can/frame_decoder.hpp"
#include "emcool/obd/stored_codes.hpp"
#include "emcool/ride_log/download.hpp"
#include "emcool/settings/dashboard_reply.hpp"
#include "emcool/signals/signal_store.hpp"

namespace {

class DiscardingRideLog final : public emcool::signals::RideLogSink {
 public:
  void appendReading(std::int64_t, std::int64_t, const emcool::signals::SignalDef&, double) override {}
};

class FixedStorage final : public emcool::ble_server::StorageReport {
 public:
  emcool::ble_server::StorageStatus storage() override {
    return {true, 3, 24ull * 1024 * 1024, 30ull * 1024 * 1024 * 1024, 32ull * 1024 * 1024 * 1024};
  }
};

// Service mode's desk, answering on its third look as a task elsewhere would, with what it was asked.
// The snapshots' directory is real and empty.
class StubServiceDesk final : public emcool::ble_server::ServiceDesk {
 public:
  explicit StubServiceDesk(std::string directory) : store_(std::move(directory), names_) {}
  bool submit(const emcool::ble_server::ServiceRequest& request) override {
    if (pending_) return false;
    pending_ = request;
    looks_ = 0;
    return true;
  }
  std::optional<emcool::ble_server::ServiceAnswer> take() override {
    if (!pending_ || ++looks_ < 3) return std::nullopt;
    const auto quoted = [](const std::optional<std::string>& value) {
      return value ? "\"" + *value + "\"" : std::string("null");
    };
    emcool::ble_server::ServiceAnswer answer{
        202, "{\"kind\":" + std::to_string(static_cast<int>(pending_->kind)) + ",\"target\":" + quoted(pending_->target) +
                 ",\"bank\":" + quoted(pending_->bank) + ",\"index\":" + quoted(pending_->index) + "}"};
    pending_.reset();
    return answer;
  }
  void abandon() override { pending_.reset(); }
  emcool::vcu::VcuStore* store() override { return &store_; }
  std::string displayDirectory() override { return "/vcu-params"; }

 private:
  emcool::vcu::ParameterNames names_;
  emcool::vcu::VcuStore store_;
  std::optional<emcool::ble_server::ServiceRequest> pending_;
  int looks_ = 0;
};

class FolderCard final : public emcool::ride_log::DownloadCard {
 public:
  explicit FolderCard(std::string root) : root_(std::move(root)) {}
  const std::string& rootDirectory() const override { return root_; }
  std::mutex& mountLock() override { return mutex_; }
  State state() override { return {true, 0, std::nullopt}; }

 private:
  std::string root_;
  std::mutex mutex_;
};

class DiscardingRecorder final : public emcool::obd::SignalRecorder {
 public:
  void record(std::string_view, double) override {}
};

class FixedTroubleCodes final : public emcool::ble_server::TroubleCodeSource {
 public:
  emcool::obd::StoredCodes troubleCodes() override { return codes; }
  emcool::obd::StoredCodes codes;
};

void writePacket(char type, const void* payload, std::size_t length) {
  const std::uint8_t header[5] = {static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(length >> 24),
                                  static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 8),
                                  static_cast<std::uint8_t>(length)};
  std::fwrite(header, 1, sizeof header, stdout);
  if (length > 0) std::fwrite(payload, 1, length, stdout);
}

bool readExactly(void* destination, std::size_t length) {
  return length == 0 || std::fread(destination, 1, length, stdin) == length;
}

std::uint32_t bigEndian32(const std::uint8_t* bytes) {
  return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) | bytes[3];
}

}  // namespace

int main(int argumentCount, char** arguments) {
  if (argumentCount < 3 || argumentCount > 5) {
    std::fprintf(stderr, "usage: ble_server_host <secret> <golden decode corpus> [<mode 03 reply hex> [<RIDES dir>]]\n");
    return 2;
  }
  if (!emcool::ble::initCrypto() || !emcool::ble::cryptoSelfTest()) {
    std::fprintf(stderr, "ble_server_host: crypto self-test failed\n");
    return 2;
  }
  std::ifstream golden(arguments[2]);
  std::vector<std::string> corpus;
  for (std::string line; std::getline(golden, line);) corpus.push_back(line);
  std::size_t nextFrame = 0;

  DiscardingRideLog rideLog;
  emcool::signals::SignalStore store(rideLog);
  std::mutex storeMutex;
  FixedStorage storage;
  FixedTroubleCodes troubleCodes;
  char directory[] = "/tmp/emcool-ble-host-XXXXXX";
  StubServiceDesk desk(::mkdtemp(directory));
  std::unique_ptr<FolderCard> rides;
  std::unique_ptr<emcool::ride_log::LogDownload> download;
  if (argumentCount == 5) {
    rides = std::make_unique<FolderCard>(arguments[4]);
    download = std::make_unique<emcool::ride_log::LogDownload>(*rides);
  }
  emcool::settings::Settings settings;
  settings.customBmsConfig = true;
  settings.obdEnabled = true;
  settings.wifiSsid = "garage \"wifi\"";
  emcool::ble_server::DashboardEndpoints endpoints(
      store, storeMutex, storage, troubleCodes, &desk, download.get(),
      emcool::settings::dashboardSettingsReply(
          settings, {"v-test", "0011223344556677", "00000000-0000-0000-0000-a1b2c3d4e5f6", download != nullptr,
                     "0123abcd-0123-4567-89ab-0123456789ab"}));
  emcool::ble_server::BleServer server(arguments[1], store, storeMutex, endpoints);
  emcool::can::FrameDecoder decoder;
  std::int64_t monotonicMs = 1'000'000;
  const auto wallClockMs = [&] { return 1767225600000LL + monotonicMs; };
  if (argumentCount >= 4 && arguments[3][0] != '\0') {
    const std::string replyHex = arguments[3];
    std::vector<std::uint8_t> reply;
    for (std::size_t offset = 0; offset + 1 < replyHex.size(); offset += 2) {
      reply.push_back(static_cast<std::uint8_t>(std::strtoul(replyHex.substr(offset, 2).c_str(), nullptr, 16)));
    }
    emcool::obd::DtcReadOutcome read;
    read.outcome = emcool::obd::DtcReadOutcome::Kind::kAnswered;
    read.mode = emcool::obd::kModeStoredDtcs;
    read.response = emcool::obd::decodeObdDtcResponse(reply, emcool::obd::kModeStoredDtcs);
    DiscardingRecorder recorder;
    troubleCodes.codes.recordRead(read, emcool::obd::DtcList::kStored, {monotonicMs, wallClockMs()}, recorder);
  }
  bool linked = false;

  while (true) {
    std::uint8_t header[5];
    if (!readExactly(header, sizeof header)) break;
    std::vector<std::uint8_t> payload(bigEndian32(header + 1));
    if (!readExactly(payload.data(), payload.size())) break;
    const char type = static_cast<char>(header[0]);

    if (type == 'C') {
      server.connected();
      server.mtuChanged(static_cast<std::uint16_t>((payload[0] << 8) | payload[1]));
      linked = true;
    } else if (type == 'D') {
      server.disconnected();
      linked = false;
    } else if (type == 'N') {
      server.notificationsEnabled(payload[0] != 0);
    } else if (type == 'W') {
      server.receiveFrame(payload, monotonicMs);
    } else if (type == 'R') {
      char info[128];
      const std::size_t length = server.info(info);
      writePacket('I', info, length);
    } else if (type == 'P') {
      const std::uint32_t count = bigEndian32(payload.data());
      for (std::uint32_t fed = 0; fed < count && nextFrame < corpus.size(); fed++, nextFrame++) {
        std::istringstream fields(corpus[nextFrame]);
        std::string idText, payloadHex;
        fields >> idText >> payloadHex;
        std::vector<std::uint8_t> bytes;
        for (std::size_t offset = 0; payloadHex != "-" && offset + 1 < payloadHex.size(); offset += 2) {
          bytes.push_back(static_cast<std::uint8_t>(std::strtoul(payloadHex.substr(offset, 2).c_str(), nullptr, 16)));
        }
        emcool::can::DecodedValues decoded;
        decoder.decode(static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 16)), bytes, decoded);
        const std::lock_guard<std::mutex> lock(storeMutex);
        for (const auto& value : decoded) {
          store.record(value.key, value.value, wallClockMs(), monotonicMs);
        }
      }
    } else if (type == 'T') {
      const std::uint32_t milliseconds = bigEndian32(payload.data());
      const std::uint32_t budget = bigEndian32(payload.data() + 4);
      std::uint32_t elapsed = 0;
      do {
        const std::uint32_t step = std::min<std::uint32_t>(10, milliseconds - elapsed);
        monotonicMs += step;
        elapsed += step;
        server.tick(monotonicMs, wallClockMs());
        std::uint8_t frame[512];
        std::size_t length = 0;
        for (std::uint32_t sent = 0; linked && sent < budget && server.nextFrame(frame, length); sent++) {
          writePacket('F', frame, length);
          server.frameSent();
          // Freed slots can take more work straight away, as on the device.
          server.tick(monotonicMs, wallClockMs());
        }
      } while (elapsed < milliseconds);
    } else if (type == 'L') {
      std::string json = "{";
      const auto registry = emcool::signals::signalRegistry();
      const std::lock_guard<std::mutex> lock(storeMutex);
      for (std::size_t index = 0; index < registry.size(); index++) {
        if (!store.seenAt(index)) continue;
        const auto live = store.liveAt(index);
        char entry[256];
        std::snprintf(entry, sizeof entry, "%s\"%s\":[%.17g,%lld]", json.size() > 1 ? "," : "", registry[index].key,
                      live.value, static_cast<long long>(live.wallClockMs));
        json += entry;
      }
      json += "}";
      writePacket('J', json.data(), json.size());
    } else if (type == 'Z') {
      const auto& counters = server.counters();
      char json[512];
      const int length = std::snprintf(
          json, sizeof json,
          "{\"sessionsStarted\":%u,\"refusals\":%u,\"problems\":%u,\"requests\":%u,"
          "\"snapshots\":%u,\"framesSent\":%llu,\"queuedBytes\":%zu,\"monotonicMs\":%lld,\"authenticated\":%s}",
          counters.sessionsStarted, counters.refusals, counters.problems, counters.requests,
          counters.snapshots, static_cast<unsigned long long>(counters.framesSent), server.queuedBytes(),
          static_cast<long long>(monotonicMs), server.authenticated() ? "true" : "false");
      writePacket('J', json, static_cast<std::size_t>(length));
    } else if (type == 'Q') {
      writePacket('K', nullptr, 0);
      std::fflush(stdout);
      break;
    }
    writePacket('K', nullptr, 0);
    std::fflush(stdout);
  }
  return 0;
}
