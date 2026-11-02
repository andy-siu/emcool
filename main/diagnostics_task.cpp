#include "diagnostics_task.hpp"

#include <algorithm>
#include <cinttypes>

#include "emcool/async/task.hpp"
#include "emcool/log.hpp"
#include "emcool/vcu/bodies.hpp"
#include "emcool/vcu/charge_auto_http.hpp"
#include "esp_app_desc.h"
#include "freertos/task.h"
#include "system_time.hpp"

namespace emcool {

namespace {

constexpr const char* kTag = "diagnostics";
constexpr std::uint32_t kStackBytes = 12288;
constexpr UBaseType_t kPriority = 4;
// Core 1, beside the decode task that feeds it; the card and the radio are core 0's.
constexpr BaseType_t kCore = 1;
constexpr UBaseType_t kQueueDepth = 64;

const char* storedState(const obd::TroubleCodeListState& list) {
  switch (list.state) {
    case obd::TroubleCodeListState::State::kNotRead:
      return "not read yet";
    case obd::TroubleCodeListState::State::kCodes:
      return "answered";
    case obd::TroubleCodeListState::State::kNoResponse:
      return "NO RESPONSE";
    case obd::TroubleCodeListState::State::kNotAsked:
      return "not asked (send failed)";
    case obd::TroubleCodeListState::State::kIncomplete:
      return "transfer stalled";
    case obd::TroubleCodeListState::State::kRefused:
      return "refused";
    case obd::TroubleCodeListState::State::kUnrecognised:
      return "unrecognised reply";
  }
  return "?";
}

}  // namespace

// The OBD side's signals land in live state as decoded ones do.
class DiagnosticsTask::StoreRecorder final : public obd::SignalRecorder {
 public:
  StoreRecorder(signals::SignalStore& store, std::mutex& storeMutex) : store_(store), storeMutex_(storeMutex) {}
  void record(std::string_view key, double value) override {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    store_.record(key, value, wallClockMs(), monotonicMs());
  }

 private:
  signals::SignalStore& store_;
  std::mutex& storeMutex_;
};

DiagnosticsTask::DiagnosticsTask(CanBus& bus, signals::SignalStore& store, std::mutex& storeMutex,
                                 const settings::Settings& settings, std::string snapshotDirectory)
    : bus_(bus),
      signalStore_(store),
      storeMutex_(storeMutex),
      settings_(settings),
      snapshotDirectory_(std::move(snapshotDirectory)),
      loop_(order_),
      recorder_(std::make_unique<StoreRecorder>(store, storeMutex)),
      store_(snapshotDirectory_, names_),
      readings_(snapshotDirectory_) {
  if (settings.obdEnabled) {
    obd_ = std::make_unique<obd::ObdStack>(bus, *recorder_, settings.elockEnabled, order_);
  }
  context_.send = [this](std::uint32_t id, const std::array<std::uint8_t, 8>& frame) { return bus_.transmit(id, frame); };
  context_.readSignal = [this](std::string_view key) {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    vcu::GateSample sample;
    if (const std::optional<signals::LiveValue> live = signalStore_.latest(key)) sample.value = live->value;
    if (const std::optional<std::int64_t> age = signalStore_.ageMs(key, monotonicMs())) {
      sample.ageMs = static_cast<double>(*age);
    }
    return sample;
  };
  context_.wallClockMs = [] { return static_cast<double>(wallClockMs()); };
  context_.hold = obd_ ? &obd_->hold() : nullptr;
  context_.store = &store_;
  // Like cool-eva's, the bus is active only with OBD_ENABLED; with it off, every read is refused.
  runner_ = std::make_unique<vcu::VcuReadRunner>(context_, lease_, settings.obdEnabled);

  // The WRITE engine, with its own switch and the read runner's gate (one opinion, not two).
  vcu::WriteContext writeContext;
  writeContext.send = context_.send;
  writeContext.enabled = settings.serviceWriteEnabled;
  writeContext.busIsActive = settings.obdEnabled;
  writeContext.directory = snapshotDirectory_;
  writeContext.gate = [this] { return runner_->gate(); };
  writeContext.readSignal = context_.readSignal;
  writeContext.wallClockMs = [] { return static_cast<double>(wallClockMs()); };
  writeContext.clockEvidence = [this] {
    vcu::PiClockEvidence evidence;
    evidence.systemEpochMs = static_cast<double>(wallClockMs());
    const vcu::GateSample gps = context_.readSignal("gps_epoch_s");
    evidence.gpsEpochSeconds = gps.value;
    evidence.gpsAgeMs = gps.ageMs;
    return evidence;
  };
  writeContext.latestReport = [this] { return store_.loadLatestTableType(); };
  const esp_app_desc_t* app = esp_app_get_description();
  writeContext.version = {std::nullopt, false, true, app != nullptr ? app->version : "unknown"};
  writeContext.hold = obd_ ? &obd_->hold() : nullptr;
  writeContext.recorder = recorder_.get();
  writeContext.stored = obd_ ? &obd_->stored() : nullptr;
  writeContext.store = &store_;
  ackWatch_ = std::make_unique<vcu::ChargeAckWatch>(*recorder_);
  writeContext.ackWatch = ackWatch_.get();
  writeContext.monotonicMs = [] { return monotonicMs(); };
  // Told BEFORE a charge current reaches the bus: a manual one stands the automatic controller
  // down for this charge. Set after chargeAuto_ exists (just below), via a trampoline.
  writeContext.onChargeCurrentOutgoing = [this](int amps, vcu::ServiceWriteRequest::ChargeOrigin origin) {
    if (chargeAuto_) chargeAuto_->noteChargeCurrentOutgoing(amps, origin == vcu::ServiceWriteRequest::ChargeOrigin::kManual);
  };
  writeRunner_ = std::make_unique<vcu::VcuWriteRunner>(std::move(writeContext), lease_);

  // The automatic controller. Its sink commands the write runner's charge-current with
  // origin=automatic; its readers come from live state on the monotonic clock.
  vcu::ChargeAutoReaders readers;
  readers.latestValue = [this](std::string_view key) -> std::optional<double> {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    if (const std::optional<signals::LiveValue> live = signalStore_.latest(key)) return live->value;
    return std::nullopt;
  };
  readers.ageMs = [this](std::string_view key) -> std::optional<double> {
    const std::lock_guard<std::mutex> lock(storeMutex_);
    if (const std::optional<std::int64_t> age = signalStore_.ageMs(key, monotonicMs())) return static_cast<double>(*age);
    return std::nullopt;
  };
  readers.nowMs = [] { return monotonicMs(); };
  vcu::ChargeCommandSink sink = [this](int amps) -> async::Task<vcu::ChargeCommandResult> {
    vcu::ServiceWriteRequest request;
    request.kind = vcu::ServiceWriteRequest::Kind::kChargeCurrent;
    request.amps = amps;
    request.origin = vcu::ServiceWriteRequest::ChargeOrigin::kAutomatic;
    const vcu::ServiceWriteAnswer answer = co_await writeRunner_->perform(std::move(request));
    if (!answer.ok) co_return vcu::ChargeCommandResult{false, answer.reason};
    co_return vcu::ChargeCommandResult{answer.result.succeeded, answer.result.message};
  };
  chargeAuto_ = std::make_unique<vcu::ChargeAutomatic>(settings.chargeAutoEnabled, std::move(sink), std::move(readers),
                                                       *recorder_);

  api_ = std::make_unique<vcu::ServiceApi>(*runner_, store_, readings_, settings.serviceModeEnabled,
                                           [] { return static_cast<double>(wallClockMs()); }, writeRunner_.get());
}

obd::Now DiagnosticsTask::now() const { return {monotonicMs(), wallClockMs()}; }

bool DiagnosticsTask::start() {
  queue_ = xQueueCreate(kQueueDepth, sizeof(Item));
  if (queue_ == nullptr) {
    logError(kTag, "no memory for the diagnostics queue");
    return false;
  }
  if (xTaskCreatePinnedToCore(run, "diagnostics", kStackBytes, this, kPriority, nullptr, kCore) != pdPASS) {
    logError(kTag, "could not create the diagnostics task (%" PRIu32 " bytes of stack)", kStackBytes);
    return false;
  }
  return true;
}

void DiagnosticsTask::forward(const ReceivedFrame& frame) {
  if (queue_ == nullptr) return;
  const Item item{false, frame};
  if (xQueueSend(queue_, &item, 0) != pdTRUE) framesDropped_++;
}

void DiagnosticsTask::observeChargeSignal(std::string_view key, double value, std::int64_t monotonicMs) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (ackWatch_ != nullptr) {
    if (key == "fast_dc_target_a") {
      ackWatch_->observeAckSignal(vcu::ChargeMode::kDc, value, monotonicMs);
    } else if (key == "charge_limit_a") {
      ackWatch_->observeAckSignal(vcu::ChargeMode::kAc, value, monotonicMs);
    } else if (key == "charge_manager_state") {
      ackWatch_->observeSessionState(value, monotonicMs);
    }
  }
  if (chargeAuto_ != nullptr) {
    if (key == "batt_temp_hi") {
      chargeAuto_->observeTemperature(value);
    } else if (key == "soc") {
      chargeAuto_->observeSoc(value);
    } else if (key == "dc_charge_limit_selected_a") {
      chargeAuto_->observeSelectedCurrent(value);
    } else if (key == "charge_manager_state") {
      chargeAuto_->observeSessionState(value);
    }
  }
}

void DiagnosticsTask::run(void* argument) { static_cast<DiagnosticsTask*>(argument)->loop(); }

void DiagnosticsTask::loop() {
  async::setCurrentLoop(&loop_);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    loop_.run(monotonicMs(), [&] { selectStartupTable(); });
    if (obd_) {
      loop_.run(monotonicMs(), [&] { obd_->start(now()); });
      logInfo("obd", "polling @2Hz (speed/rpm/temps/load/distance)");
    }
    logInfo("service-mode", "%s",
            settings_.serviceModeEnabled
                ? "the dashboard may start an on-demand VCU parameter read while the bike is parked and out of drive "
                  "(set SERVICE_MODE_ENABLED 0 to forbid it)"
                : "disabled (SERVICE_MODE_ENABLED=0) — the snapshot is still served and exported, but nothing here "
                  "can ask the bike");
    if (settings_.serviceWriteEnabled) {
      logWarning("service-write",
                 "⚠️  ENABLED (SERVICE_WRITE_ENABLED=1) — the dashboard may write allowlisted VCU parameters, set the "
                 "service point, sync the clock, clear DTCs, command the charge current/stop and set the charge "
                 "limit. Every attempt is journalled to service-writes.jsonl. Set SERVICE_WRITE_ENABLED 0 to forbid "
                 "it.");
    } else {
      logInfo("service-write",
              "disabled (default) — nothing here can change anything on the bike. Set SERVICE_WRITE_ENABLED 1 to allow "
              "it.");
    }
    if (chargeAuto_) loop_.run(monotonicMs(), [&] { chargeAuto_->start(); });
    if (settings_.chargeAutoEnabled) {
      logWarning("charge-auto",
                 "⚠️  ENABLED (CHARGE_AUTO_ENABLED=1) — the controller may command the DC charge current on a timer "
                 "while a fast charge runs. It needs SERVICE_WRITE_ENABLED to actuate; without it, commands are "
                 "refused. Set CHARGE_AUTO_ENABLED 0 to forbid it.");
    }
  }
  while (true) {
    std::optional<std::int64_t> next;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const std::optional<obd::Deadline> timer = loop_.nextTimer();
      const std::optional<obd::Deadline> deadline = obd_ ? obd_->nextDeadline() : std::nullopt;
      if (const std::optional<obd::Deadline> earliest = obd::earliest(timer, deadline)) next = earliest->atMs;
    }
    const std::int64_t waitMs = next ? std::clamp<std::int64_t>(*next - monotonicMs(), 0, 1000) : 1000;
    Item item;
    if (xQueueReceive(queue_, &item, static_cast<TickType_t>((waitMs + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS)) ==
        pdTRUE) {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (item.command) {
        std::optional<ble_server::ServiceRequest> request;
        std::uint32_t serial = 0;
        {
          const std::lock_guard<std::mutex> desk(deskMutex_);
          request = std::exchange(request_, std::nullopt);
          serial = requestSerial_;
        }
        if (request) loop_.run(monotonicMs(), [&] { handle(*request, serial); });
      } else {
        loop_.run(monotonicMs(), [&] { route(item.frame); });
      }
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    fireDue();
  }
}

// The loop's timers and the OBD side's deadlines, together, in the order they were set.
void DiagnosticsTask::fireDue() {
  const std::int64_t nowMs = monotonicMs();
  for (int fired = 0; fired < 256; fired++) {
    const std::optional<obd::Deadline> timer = loop_.nextTimer();
    const std::optional<obd::Deadline> deadline = obd_ ? obd_->nextDeadline() : std::nullopt;
    const std::optional<obd::Deadline> next = obd::earliest(timer, deadline);
    if (!next || next->atMs > nowMs) return;
    if (timer && timer->order == next->order) {
      loop_.fireNext(nowMs);
    } else {
      loop_.run(nowMs, [&] { obd_->fireNext(now()); });
    }
  }
}

void DiagnosticsTask::route(const ReceivedFrame& frame) {
  const std::span<const std::uint8_t> data(frame.data, frame.length);
  const std::optional<double> arrival = static_cast<double>(frame.arrivedUs) / 1000.0;
  if (obd::ObdPoller::isResponse(frame.id)) {
    // Service mode's KWP replies share 0x7E0 with OBD's; it takes only those addressed to 0xF1.
    if (runner_->handleCanFrame(frame.id, data, arrival)) return;
    // A write in flight needs the OBD range too: KWP legs answer on 0x7E0, and clear-dtcs reads
    // PIDs and Mode 04 in 0x7E0-0x7EF. Only one runner is busy at a time (the bus lease).
    if (writeRunner_->handleCanFrame(frame.id, data)) return;
    if (obd_) obd_->handleFrame(frame.id, data, now());
  } else if (obd::ElockReader::isResponse(frame.id) && obd_) {
    obd_->handleFrame(frame.id, data, now());
  }
}

// cool-eva's index.ts: the names come from the table the last snapshot says the bike runs.
void DiagnosticsTask::selectStartupTable() {
  const std::optional<vcu::TableTypeReport> report = store_.loadLatestTableType();
  const std::string active = vcu::describeTableType(names_.active().tableType);
  const std::size_t carried = vcu::parameterTables().size();
  if (!report) {
    logInfo("vcu-table", "no parameter snapshot in %s, so parameters are named from %s until a sweep says otherwise. %zu tables carried.",
            snapshotDirectory_.c_str(), active.c_str(), carried);
    return;
  }
  if (!report->confirmed) {
    for (const std::string& line : report->lines) logWarning("vcu-table", "%s", line.c_str());
  }
  if (report->mismatched || report->split) {
    logWarning("vcu-table", "the tables this build carries, for comparison: %s", vcu::knownTableTypesText().c_str());
  }
  if (!report->tableType) {
    logInfo("vcu-table",
            "nothing in %s names one parameter table this software carries, so parameters are named from %s until a "
            "sweep says otherwise. %zu tables carried.",
            snapshotDirectory_.c_str(), active.c_str(), carried);
    return;
  }
  names_.select(static_cast<int>(*report->tableType));
}

void DiagnosticsTask::answer(std::uint32_t serial, int status, std::string body) {
  const std::lock_guard<std::mutex> desk(deskMutex_);
  // An answer nobody is waiting for any more: its phone went, and abandon() already freed the desk.
  if (serial == abandonedSerial_ || serial != requestSerial_) return;
  answer_ = ble_server::ServiceAnswer{status, std::move(body)};
}

void DiagnosticsTask::handle(const ble_server::ServiceRequest& request, std::uint32_t serial) {
  using Kind = vcu::ServiceRequest::Kind;
  // The automatic-charge controller is the task's own, not the VCU read/write runners'; answer its
  // two endpoints here, synchronously on this loop, rather than through the ServiceApi.
  if (request.kind == Kind::kChargeAutoStatus || request.kind == Kind::kChargeAutoSet) {
    const auto build = [](const std::function<void(text::TextBuffer&)>& write) {
      text::TextBuffer measure = text::TextBuffer::window(nullptr, 0, 0);
      write(measure);
      std::string text(measure.total(), '\0');
      text::TextBuffer out(text.data(), text.size());
      write(out);
      text.resize(out.length());
      return text;
    };
    if (chargeAuto_ == nullptr) {
      answer(serial, 503, "{}");
      return;
    }
    std::optional<std::string> message;
    int status = 200;
    if (request.kind == Kind::kChargeAutoSet) {
      const std::string mode = request.target.value_or("");  // reuse `target` for the mode query
      if (mode != "automatic" && mode != "off") {
        answer(serial, 400,
               build([&](text::TextBuffer& out) {
                 vcu::writeChargeAutoResponseJson(out, chargeAuto_->state(),
                                                  "mode must be \"automatic\" or \"off\".");
               }));
        return;
      }
      chargeAuto_->setMode(mode == "automatic" ? vcu::ChargeAutoMode::kAutomatic : vcu::ChargeAutoMode::kOff);
      const vcu::ChargeAutoState after = chargeAuto_->state();
      const bool refused = (mode == "automatic") != (after.mode == vcu::ChargeAutoMode::kAutomatic);
      if (refused) message = "CHARGE_AUTO_ENABLED is 0 on this bike, so the controller stays off.";
    }
    answer(serial, status, build([&](text::TextBuffer& out) {
             vcu::writeChargeAutoResponseJson(out, chargeAuto_->state(), message);
           }));
    return;
  }
  [](DiagnosticsTask* self, ble_server::ServiceRequest asked, std::uint32_t serial) -> async::Task<> {
    vcu::ServiceAnswer answered = co_await self->api_->answer(std::move(asked));
    self->answer(serial, answered.status, std::move(answered.body));
  }(this, request, serial)
      .detach();
}

bool DiagnosticsTask::submit(const ble_server::ServiceRequest& request) {
  {
    const std::lock_guard<std::mutex> desk(deskMutex_);
    if (busy_) return false;
    busy_ = true;
    requestSerial_++;
    request_ = request;
    answer_.reset();
  }
  // A full queue only delays it: the loop looks for a waiting request on its next pass anyway.
  const Item item{true, {}};
  xQueueSend(queue_, &item, 0);
  return true;
}

std::optional<ble_server::ServiceAnswer> DiagnosticsTask::take() {
  const std::lock_guard<std::mutex> desk(deskMutex_);
  if (!answer_) return std::nullopt;
  busy_ = false;
  return std::exchange(answer_, std::nullopt);
}

void DiagnosticsTask::abandon() {
  const std::lock_guard<std::mutex> desk(deskMutex_);
  abandonedSerial_ = requestSerial_;
  request_.reset();
  answer_.reset();
  busy_ = false;
}

obd::StoredCodes DiagnosticsTask::troubleCodes() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return obd_ ? obd_->stored() : obd::StoredCodes{};
}

DiagnosticsSummary DiagnosticsTask::summary() {
  const std::lock_guard<std::mutex> lock(mutex_);
  DiagnosticsSummary summary;
  summary.framesDropped = framesDropped_;
  summary.sweepRunning = runner_->running();
  if (!obd_) return summary;
  summary.obd = true;
  summary.rounds = obd_->poller().rounds();
  const obd::TroubleCodeListState& stored = obd_->stored().list(obd::DtcList::kStored);
  summary.storedCodes = storedState(stored);
  if (stored.state == obd::TroubleCodeListState::State::kCodes) {
    summary.storedCodes += " (" + std::to_string(stored.codes.size()) + ")";
  }
  summary.heldBy = std::string(obd_->hold().heldBy());
  return summary;
}

}  // namespace emcool
