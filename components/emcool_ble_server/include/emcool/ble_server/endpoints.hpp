#pragma once

// The dashboard's requests. cool-eva replays each BLE request into its own HTTP server; emcool
// has none, so requests are answered here directly, with cool-eva's bodies:
//   GET /status          cool-eva's StatusPayload shape (src/http/status.ts)
//   GET /dtc-table       Energica's code table, byte for byte cool-eva's
//   GET /fault-infokeys  Energica's per-fault telemetry shortlists, likewise
//   GET /stored-dtcs     the OBD trouble-code lists as last read (all "not-read" with OBD off)
//   GET /dl              the ride log off the card: the newest parts, up to ?mb= (default 16, at
//                        most 256), or only ?boot=N's — see ride_log/download.hpp
//   GET /settings        cool-eva's SettingsReply shape, read-only: what this boot runs with, from
//                        the serial console's settings (settings/dashboard_reply.hpp). Changing
//                        one stays a console job; a PATCH gets a 405 saying so
//   POST /settings/bluetooth-key  cool-eva's Show key read-back, refused: emcool never returns
//                        its Bluetooth key (docs/ble.md)
//   /console, /console/status  the serial console's commands and status report, for Settings →
//                        Bike console (console_endpoints.hpp). The one way here to change a setting
// Everything else is a 404 saying the endpoint isn't on emcool yet. None of these touches the
// bus: like cool-eva's, they serve what is already known, so refreshing a page can't make the
// bike answer anything.
//
// A body is read a piece at a time (the server sends 4 KB per message), so one far bigger
// than any buffer here — /dtc-table is 25 KB — comes straight out of flash, and /stored-dtcs
// is written out again for each piece from a copy taken when the request arrived, so its
// pieces always agree.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "emcool/ble_server/console_endpoints.hpp"
#include "emcool/obd/stored_codes.hpp"
#include "emcool/ride_log/download.hpp"
#include "emcool/signals/signal_store.hpp"
#include "emcool/vcu/bodies.hpp"
#include "emcool/vcu/service_api.hpp"
#include "emcool/vcu/store.hpp"

namespace emcool::ble_server {

struct HttpResponse {
  int status;
  const char* contentType;
  std::size_t bodyLength;
  // A Content-Disposition attachment name, for a body meant to be saved; valid until the next begin().
  const char* fileName = nullptr;
};

struct HttpRequest {
  std::string_view method;
  std::string_view path;  // with its query
  // The X-Cool-Eva header: what a request that changes something is for ("service-mode", …).
  std::string_view coolEva;
  // What came with it, if anything; valid only during begin().
  std::string_view body = {};
};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  // Starts answering a request: its status, type and whole length — or nullopt if the answer
  // comes later, from poll(). One response at a time; a new begin() ends the last.
  virtual std::optional<HttpResponse> begin(const HttpRequest& request, std::int64_t nowMonotonicMs) = 0;
  // A later answer, once there is one.
  virtual std::optional<HttpResponse> poll(std::int64_t) { return timedOut(); }
  // No answer in time: what to send instead. And the phone went away before it came.
  virtual HttpResponse timedOut() { return {504, "text/plain; charset=utf-8", 0}; }
  // The phone went away (or a new session began) with the response unfinished, waiting or partly sent.
  virtual void abandon() {}
  // Up to `maxBytes` of the body from `offset`; valid until the next call.
  virtual std::span<const char> body(std::size_t offset, std::size_t maxBytes) = 0;
};

// Service mode's requests, answered by the task that owns the bus (cool-eva's read runner).
using ServiceRequest = vcu::ServiceRequest;
using ServiceAnswer = vcu::ServiceAnswer;
class ServiceDesk {
 public:
  virtual ~ServiceDesk() = default;
  // Hands a request over; false if one is already waiting.
  virtual bool submit(const ServiceRequest& request) = 0;
  // Its answer, once there is one.
  virtual std::optional<ServiceAnswer> take() = 0;
  // Forgets a request whose phone went away.
  virtual void abandon() = 0;
  // Where the snapshots are, for /vcu-params and /vcu-backup, read from here; null without a card.
  virtual vcu::VcuStore* store() = 0;
  // What the snapshots' directory is called in a body.
  virtual std::string displayDirectory() = 0;
};

struct StorageStatus {
  bool logging;
  std::uint32_t files;
  std::uint64_t bytes;
  std::optional<std::uint64_t> freeBytes;
  std::optional<std::uint64_t> totalBytes;
};

class StorageReport {
 public:
  virtual ~StorageReport() = default;
  virtual StorageStatus storage() = 0;
};

// The trouble-code lists, copied out from under whatever lock guards them.
class TroubleCodeSource {
 public:
  virtual ~TroubleCodeSource() = default;
  virtual obd::StoredCodes troubleCodes() = 0;
};

class DashboardEndpoints final : public RequestHandler {
 public:
  // cool-eva's FRESH_MS: a signal counts as live in /status if it arrived this recently.
  static constexpr std::int64_t kFreshMs = 10000;
  // The largest body built whole (/status, the 404s), and the largest piece handed out: one
  // response piece (BleServer::kResponseChunkBytes) at a time.
  static constexpr std::size_t kBufferBytes = 4 * 1024;

  // How many of the download's planning steps one poll may take. Each lists a directory or bisects
  // one part — a few dozen sector reads — and the link task sends nothing while it runs.
  static constexpr int kDownloadStepsPerPoll = 1;

  // `settingsBody`: /settings's JSON, built once at boot (the settings it describes can't change
  // until a restart); empty answers 404, as on a bike with no such endpoint.
  // `console`: /console's commands; null answers 404, as on a bike without them.
  DashboardEndpoints(const signals::SignalStore& store, std::mutex& storeMutex, StorageReport& storage,
                     TroubleCodeSource& troubleCodes, ServiceDesk* service = nullptr,
                     ride_log::LogDownload* download = nullptr, std::string settingsBody = {},
                     ConsoleEndpoints* console = nullptr)
      : store_(store),
        storeMutex_(storeMutex),
        storage_(storage),
        troubleCodes_(troubleCodes),
        service_(service),
        download_(download),
        settingsBody_(std::move(settingsBody)),
        console_(console) {}

  std::optional<HttpResponse> begin(const HttpRequest& request, std::int64_t nowMonotonicMs) override;
  std::optional<HttpResponse> poll(std::int64_t nowMonotonicMs) override;
  HttpResponse timedOut() override;
  void abandon() override;
  std::span<const char> body(std::size_t offset, std::size_t maxBytes) override;

 private:
  enum class Source : std::uint8_t { kBuffer, kStatic, kStoredCodes, kParts, kText, kDownload };
  // What a request answered later is waiting on.
  enum class Waiting : std::uint8_t { kNothing, kService, kDownload, kConsole };

  HttpResponse status(std::int64_t nowMonotonicMs);
  HttpResponse fromBuffer(int status, const char* contentType, std::size_t length);
  HttpResponse fromText(int status, const char* contentType, std::string_view text);
  std::optional<HttpResponse> service(const HttpRequest& request, std::string_view route, std::string_view query);
  HttpResponse fromParts(int status, const char* contentType, std::unique_ptr<vcu::PartsBody> parts);
  std::optional<HttpResponse> beginDownload(std::string_view query);
  std::optional<HttpResponse> pollDownload();
  void endDownload();

  const signals::SignalStore& store_;
  std::mutex& storeMutex_;
  StorageReport& storage_;
  TroubleCodeSource& troubleCodes_;
  ServiceDesk* service_;
  ride_log::LogDownload* download_;
  const std::string settingsBody_;
  ConsoleEndpoints* console_;
  Waiting waiting_ = Waiting::kNothing;
  std::string downloadName_;
  // A body read a part at a time: the parts, and where the one in hand sits in the body.
  std::unique_ptr<vcu::PartsBody> parts_;
  std::string part_;
  std::size_t partOffset_ = 0;
  std::string text_;

  Source source_ = Source::kBuffer;
  std::size_t length_ = 0;
  std::string_view static_;
  obd::StoredCodes codes_;
  std::int64_t codesAtMs_ = 0;
  std::array<char, kBufferBytes> buffer_ = {};
};

}  // namespace emcool::ble_server
