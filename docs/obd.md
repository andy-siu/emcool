# OBD polling

emcool's port of cool-eva's OBD side: the mode 01 PID poller, the trouble-code reads, the poller's hold, and the one-shot E-LOCK read. The code is in `components/emcool_obd/`, ported from cool-eva's `src/can/obd.ts`, `obd-dtc.ts`, `obd-hold.ts`, `elock.ts` and `iso-tp.ts`, and `src/diagnostics/obd-dtc.ts`, `stored-codes.ts` and `dtc-table.ts`.

## Off by default

cool-eva polls OBD unless `OBD_ENABLED=0`. emcool doesn't poll unless `OBD_ENABLED` is 1:

```
emcool> set OBD_ENABLED 1
emcool> set ELOCK_ENABLED 1      optional; needs OBD_ENABLED
emcool> restart
```

With it off, the CAN controller is listen-only. It sends nothing at all, not even the acknowledge bit, so the bike's bus can't tell it's there.

With it on, the controller is an ordinary node on the bus, as cool-eva's `can0` is when active:

- **It acknowledges every frame,** like any other node.
- **It retries a frame until something acknowledges it,** as SocketCAN does. The controller's zeroed default is single-shot, which would drop a request whenever it lost arbitration to the bike's own traffic.
- **It recovers from bus-off** by itself, at most once a second.

## The hub's active faults

Separately from OBD, and passive, emcool reads the bike's active fault list as the Energica Connectivity Hub mirrors it onto 0x410 (message type 25). This is a port of cool-eva's `src/can/hub-mirror.ts` and `src/diagnostics/decode.ts` and `record.ts`. It needs no setting, because it only listens.
- **Signals for each code:** each complete list sets `dtc_CCCC_S` to 1 for a code present, and to 0 for one seen before and now gone.
- **Totals:** `dtc_list_count` and `dtc_unrecognised_count`.
- **Display:** the Faults tab names the codes from `/dtc-table`.

`host/tests/check_diagnostics.cpp` holds it to cool-eva's code over 2,436 frames, every signal and log line included.

## What it sends

All on the functional address 0x7DF unless noted, 8 bytes each:

- **Mode 01 PIDs**, one at a time, each waited for up to 200 ms, with a round every 500 ms:
  - every round: speed, motor rpm, the three temperatures, 12 V, SOH, load and distance since the codes were cleared
  - every 20th round: the diagnostic PIDs, which are the MIL, the freeze-frame code, and the time and distance since clearing and with the MIL on
- **Mode 03, the stored trouble codes,** every 120th round, about once a minute. Every tenth time, it also asks mode 07 (pending) and mode 0A (permanent). Those have never answered on cool-eva's bike.
- **Flow control** to 0x7E0 (the responder's id less 8), when a trouble-code reply needs more than one frame.
- **The E-LOCK keys-paired read**, once at startup with `ELOCK_ENABLED`: `21 99` to 0x791. If that gets no usable answer, it opens a standard session (`10 81`), asks again, and closes the session (`20`).

The replies, 0x7E0–0x7EF and 0x790, go to the OBD side and never to the broadcast decoders, whether or not OBD is on. That matches cool-eva.

## What it records

- **Signals:** the PIDs' values, `dtc_stored_count` (and pending and permanent) when the bike answers, `freeze_frame_dtc`, and `keys_paired`.
  - These go into live state and the ride log like any decoded signal.
- **`/stored-dtcs`:** the three lists, in cool-eva's `TroubleCodeSnapshot` shape.
  - Each list is one of `codes` (the bike answered, perhaps with none), `no-response`, `incomplete` (a transfer started and stalled on every attempt), `refused`, `not-asked` (the send failed on our side) or `not-read`.
  - The codes are named from Energica's table.
  - **Silence is never shown as "no codes":** a count is recorded only when the bike answered.
- **The console log:** each list when it first appears or changes, signed on the codes themselves, not on the count.

## The hold

Anything that needs the bus to itself asks the poller to park. Service mode's A8 firmware-table reads and its freeze-frame reads do, and so does the trouble-code clear (`clear-dtcs`), which parks the poller around its Mode 04 and the PID reads that prove the erasure. The VCU parameter writes and the other service actions answer on `0x7E0` addressed to `0xF1`, which no OBD reply is, so they share the bus with the poller rather than parking it.

- **When it parks:** at its next check, before each PID, before each trouble-code read, and at the start of each round.
- **If it won't park:** the asker gives up after 6 s.
- **If the owner holds too long:** the poller takes the bus back after 15 s, however the owner is doing.

## How it runs

cool-eva's versions are async functions on Node timers. emcool's are state machines the decode task advances:

- Each frame from 0x7E0–0x7EF or 0x790 is passed to `handleFrame()`.
- The task calls `tick()` after every frame.
- The task waits for the next frame no longer than the OBD side's next deadline.

Two deadlines that fall in the same millisecond fire in the order they were set, as Node runs timers. It matters for one race: a hold's wait running out in the same millisecond as the poller's next check. Whichever fires first decides whether the hold is granted.

FreeRTOS ticks at 100 Hz, so a timeout can run up to 10 ms long on the bike. That's well inside every margin here.

Memory: the OBD side is about 2 KB, mostly the ISO-TP reassembly buffer. It's allocated only when `OBD_ENABLED` is on.

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **The pure parts.** `tools/generate-golden-obd.mts` checks these exhaustively or by fuzzing:
  - code formatting and table lookup for all 65,536 codes
  - every NRC's name
  - 94,000 ISO-TP frames in random sequences
  - 4,000 random replies through the trouble-code decoder
- **The poller, the reads, the hold and the E-LOCK read,** against a scripted ECU on a fake clock, in 72 scenarios covering four simulated hours. The ECU's behaviour includes:
  - PIDs answered, late, short, twice or not at all
  - trouble-code lists as single frames, or as multi-frame transfers that complete, stall, skip a sequence number, restart or overflow
  - refusals, silence, and flow control to an id the poller must refuse
  - noise on the diagnostic ids, and sends that fail
  - holds that are granted, refused, released, or outstay their cap, including both orders of the same-millisecond race

  `host/tests/check_obd.cpp` replays each scenario through the C++. In every millisecond it must send the same frames, record the same signals, log the same lines and serve the same `/stored-dtcs` body as cool-eva's own code did.
- **The test can fail.** It catches each of these mutations:
  - a PID timeout 1 ms short
  - one retry fewer
  - a swapped DTC letter
  - no change detection on the log
  - an off-by-one hold expiry
  - a skipped session close
  - the 1 ms minimum sleep removed
  - consecutive frames not recognised
  - deadlines ordered by time alone
  - a read time stamped after a failed send
  - a windowing bug in the body writer
  - the ISO-TP frame cap not counting
  - padding read as a code
  - the wrong round for the stored read
  - `truncated` written as a number

**Not verified yet** (nothing has run on the ESP32):

- **Transmitting through the TWAI driver and the MCP2562,** and the bus-off recovery. Receiving at 500 kbit already needs the transceiver out of standby, so once listening works, transmitting should too.
- **That the bike answers emcool as it answers cool-eva.** The requests are byte-identical, so it should.
