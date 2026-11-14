#pragma once

// The task that talks to the bike: the OBD poller (with OBD_ENABLED) and service mode's read runner
// on one event loop. The decode task hands it the diagnostic replies (0x7E0-0x7EF and 0x790); the
// Bluetooth task hands it the dashboard's service requests and takes the answers back when they're
// ready, so a probe that takes seconds never holds up the live stream; and it writes service mode's
// snapshots to the card itself, so a slow card never holds up the decoding.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "can_bus.hpp"
#include "emcool/async/loop.hpp"
#include "emcool/ble_server/endpoints.hpp"
#include "emcool/obd/obd_stack.hpp"
#include "emcool/settings/settings.hpp"
#include "emcool/signals/signal_store.hpp"
#include "emcool/vcu/reading_stores.hpp"
#include "emcool/vcu/charge_auto.hpp"
#include "emcool/vcu/runner.hpp"
#include "emcool/vcu/service_api.hpp"
#include "emcool/vcu/store.hpp"
#include "emcool/vcu/write_runner.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace emcool {

struct DiagnosticsSummary {
  bool obd = false;
  std::uint32_t rounds = 0;
  std::string storedCodes;
  std::string heldBy;
  bool sweepRunning = false;
  // The poller exists (OBD on) but is stopped by the console's `poller off`.
  bool pollerStopped = false;
  std::uint32_t framesDropped = 0;
  // What holds the bus for service mode (a parameter read, a service write), or empty.
  std::string busHeldBy;
};

class DiagnosticsTask final : public ble_server::ServiceDesk, public ble_server::TroubleCodeSource {
 public:
  DiagnosticsTask(CanBus& bus, signals::SignalStore& store, std::mutex& storeMutex,
                  const settings::Settings& settings, std::string snapshotDirectory);

  bool start();
  // From the decode task: a diagnostic reply, never blocking it.
  void forward(const ReceivedFrame& frame);
  // From the decode task: the charge ack signals (fast_dc_target_a, charge_limit_a) and
  // charge_manager_state, so the charge-current ack-watch sees them as they arrive.
  void observeChargeSignal(std::string_view key, double value, std::int64_t monotonicMs);

  // ble_server::ServiceDesk, from the Bluetooth task.
  bool submit(const ble_server::ServiceRequest& request) override;
  std::optional<ble_server::ServiceAnswer> take() override;
  void abandon() override;
  vcu::VcuStore* store() override { return &store_; }
  std::string displayDirectory() override { return snapshotDirectory_; }

  // ble_server::TroubleCodeSource: /stored-dtcs' copy.
  obd::StoredCodes troubleCodes() override;

  // For the status report.
  DiagnosticsSummary summary();

  // The USB console's `poller` command, from its own task. Stops or restarts the OBD poller.
  // false when OBD is off, so there is no poller to touch.
  bool setPollerRunning(bool run);
  // OBD on, and the poller not stopped by the console. nullopt when OBD is off.
  std::optional<bool> pollerRunning();

 private:
  struct Item {
    bool command;
    ReceivedFrame frame;
  };
  class StoreRecorder;

  static void run(void* argument);
  void loop();
  void route(const ReceivedFrame& frame);
  void fireDue();
  void handle(const ble_server::ServiceRequest& request, std::uint32_t serial);
  void answer(std::uint32_t serial, int status, std::string body);
  void selectStartupTable();
  obd::Now now() const;

  CanBus& bus_;
  signals::SignalStore& signalStore_;
  std::mutex& storeMutex_;
  const settings::Settings& settings_;
  std::string snapshotDirectory_;
  QueueHandle_t queue_ = nullptr;
  std::uint32_t framesDropped_ = 0;

  // Everything below is the task's own, except under mutex_ for troubleCodes() and summary().
  std::mutex mutex_;
  obd::DeadlineOrder order_;
  async::Loop loop_;
  std::unique_ptr<StoreRecorder> recorder_;
  std::unique_ptr<obd::ObdStack> obd_;
  vcu::ParameterNames names_;
  vcu::VcuStore store_;
  vcu::ServiceContext context_;
  vcu::BusLease lease_;
  std::unique_ptr<vcu::VcuReadRunner> runner_;
  std::unique_ptr<vcu::ChargeAckWatch> ackWatch_;
  std::unique_ptr<vcu::VcuWriteRunner> writeRunner_;
  std::unique_ptr<vcu::ChargeAutomatic> chargeAuto_;
  vcu::ReadingStore readings_;
  std::unique_ptr<vcu::ServiceApi> api_;

  // The desk: one request at a time, and its answer, under deskMutex_.
  std::mutex deskMutex_;
  std::optional<ble_server::ServiceRequest> request_;
  std::uint32_t requestSerial_ = 0;
  std::uint32_t abandonedSerial_ = 0;
  std::optional<ble_server::ServiceAnswer> answer_;
  bool busy_ = false;
};

}  // namespace emcool
