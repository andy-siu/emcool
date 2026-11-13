#pragma once

// Everything emcool says over OBD, in one place for the decode task to drive: the poller, the
// trouble-code reader it uses, the stored-codes state behind /stored-dtcs, the poller's hold,
// and the one-shot E-LOCK read. It exists only with OBD_ENABLED.
//
// Routing: 0x7E0-0x7EF and 0x790 are the diagnostic replies and are never passed to the
// broadcast decoders, whether or not OBD is on.

#include <cstdint>
#include <optional>
#include <span>

#include "emcool/obd/bus.hpp"
#include "emcool/obd/elock.hpp"
#include "emcool/obd/obd_hold.hpp"
#include "emcool/obd/obd_poller.hpp"
#include "emcool/obd/stored_codes.hpp"
#include "emcool/obd/transmit_log.hpp"
#include "emcool/obd/trouble_code_reader.hpp"

namespace emcool::obd {

// A diagnostic reply: for the OBD side, not the decoders.
inline bool isDiagnosticReply(std::uint32_t id) { return ObdPoller::isResponse(id) || ElockReader::isResponse(id); }

class ObdStack {
 public:
  ObdStack(CanTransmitter& bus, SignalRecorder& recorder, bool elockEnabled)
      : ObdStack(bus, recorder, elockEnabled, ownOrder_) {}
  // Sharing `order` with an event loop, so ties between its timers and these keep the order set.
  ObdStack(CanTransmitter& bus, SignalRecorder& recorder, bool elockEnabled, DeadlineOrder& order)
      : hold_(order),
        reader_(bus, order, transmitLogs_.troubleCodes),
        poller_(bus, recorder, reader_, stored_, hold_, order, transmitLogs_.obd),
        elock_(bus, recorder, order),
        elockEnabled_(elockEnabled) {}

  // Starts polling, and the E-LOCK read if it's on, in that order.
  void start(Now now);
  // A diagnostic reply (isDiagnosticReply).
  void handleFrame(std::uint32_t id, std::span<const std::uint8_t> data, Now now);
  // Fires every deadline that's due, earliest (and first set) first.
  void tick(Now now);
  // The earliest deadline, and firing just that one if it's due: for a loop with timers of its own
  // to interleave in the same order.
  std::optional<Deadline> nextDeadline() const;
  bool fireNext(Now now);
  // When tick() is next needed, if at all.
  std::optional<std::int64_t> nextDeadlineMs() const;

  ObdHold& hold() { return hold_; }
  const ObdHold& hold() const { return hold_; }
  TroubleCodeReader& reader() { return reader_; }
  StoredCodes& stored() { return stored_; }
  const StoredCodes& stored() const { return stored_; }
  const ObdPoller& poller() const { return poller_; }
  TransmitLogs& transmitLogs() { return transmitLogs_; }

 private:
  DeadlineOrder ownOrder_;
  TransmitLogs transmitLogs_;
  ObdHold hold_;
  TroubleCodeReader reader_;
  StoredCodes stored_;
  ObdPoller poller_;
  ElockReader elock_;
  const bool elockEnabled_;
};

}  // namespace emcool::obd
