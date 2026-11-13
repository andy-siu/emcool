# Service mode: reading the VCU's parameters

emcool's service mode reads:
- **The sweep:** reads every calibration parameter off the VCU's two micros, A9 and A8, into a snapshot the dashboard's parameters page shows and exports.
- **The probe:** reads one identifier from any bank.

The code is in `components/emcool_vcu/`.

The read side only reads. Its encoders build five requests and check the service byte against that list again:
- `10 81`, `3E` and `22 hi lo`, for the parameters
- `17 <component>` and `18 02 FF FF`, for freeze frames

Writing is a separate engine behind its own switch — see [Writing to the VCU](#writing-to-the-vcu) below.

## Freeze frames and the lifetime statistics

Two more reads come with service mode.

- **The lifetime statistics** (`/lifetime-read`, then `/lifetime-stats`) read freeze-frame components 51 and 52 off the A8. These are the odometer, SOH and cells, then the charge counters.
- **The stored freeze frames** (`/freeze-frame-read`, then `/freeze-frames`) are what the VCU recorded when each stored code set:
  - first the `18` list of components that hold one
  - then `17` for each, inside a 12 s budget so the read fits the poller's 15 s hold

Both park the OBD poller and run under the same gate as a probe.

Freeze frames are decoded with Energica's 120-field infokey table and per-fault shortlists. `tools/generate-diagnostic-tables.mts` generates them from the reference implementation. Readings no sensor could give are rejected, not shown.

The readings are kept beside the snapshots:
- **Files:** `lifetime.json` and `freeze-frames.json` for the latest, and `lifetime-<ISO>.json` and `freeze-frames-<ISO>.json` for every read.
- **Replacing:** a read that answered less than the stored one doesn't replace it.

`/expected-faults` keeps the codes this bike is expected to have, in `expected-faults.json`. It's a display preference: no bus, no gate.

## Turning it on

Off by default.

```
emcool> set OBD_ENABLED 1            the bus must be active
emcool> set SERVICE_MODE_ENABLED 1
emcool> restart
```

- **With `SERVICE_MODE_ENABLED` 0:**
  - The dashboard still shows the last snapshot and exports it.
  - Starting a read or a probe answers 403.
- **With `OBD_ENABLED` 0:** a read is refused, because the bus is listen-only.

## When it reads

Only while the bike is safe to service. That's the service gate, every reading fresh:
- **Stopped:** `speed_can_kmh`, `motor_rpm_can` and `moving` all read 0, and the OBD speed agrees when there is one.
- **Out of drive:** `go`, `go_request` and `throttle_on` all read 0.
- **Not energized:** `energized` reads 0, unless a charge is live.

The runner keeps checking every 200 ms while it works. The moment the bike isn't safe, the sweep stops, keeping what it has read.

A sweep doesn't stop the OBD poller: KWP replies on 0x7E0 are addressed to 0xF1, which no OBD reply is. It parks the poller only for the A8 rows whose width only the A8's firmware table gives. A probe always parks it.

## What it keeps on the card

In `/sdcard/vcu-params/`:

| File | What |
| --- | --- |
| `sweep.partial.jsonl` | Every row as it's read, `{"at":…, …row}`. This is the resume file, so a stopped sweep picks up where it left off. emcool also writes each snapshot from it, so a sweep never holds its 302 rows in memory. |
| `<ISO time>.jsonl` | Each snapshot: a header line, then one row per line, each written as JavaScript's `JSON.stringify` writes a row. Never rewritten. |
| `latest.txt` | The name of the snapshot the dashboard is shown. It's a pointer, so the phone never reads a file that's being replaced. A sweep moves it only if it read at least as much as the last. |

A snapshot is kept as JSON lines, not one JSON document, because a 100 KB document doesn't fit the ESP32's heap. The dashboard's bodies are written out a row at a time. When a snapshot is loaded, its rows are re-named from the parameter table the bike reports.

## Parameter tables

emcool carries a catalogue of 29 tables. `tools/generate-vcu-tables.mts` generates it from the reference implementation's table catalogue; 21 tables are distinct, about 70 KB of flash.

The bike's own TABLE_TYPE (indices 276 and 277) picks which table names the parameters:
- **At startup:** from the latest snapshot.
- **After each sweep:** from what the sweep read.

A table emcool doesn't carry is reported, never guessed. Until it's added, the names shown may be wrong for the bike, and the table-type gate refuses parameter writes. To add one, extract it from an Energica service-tool install with the reference implementation's extract script, run on the tool's parameter-definition assembly. The script merges the new tables into the reference's catalogue. Then re-run the generator, which rewrites `components/emcool_vcu/src/tables.gen.cpp`, and rebuild:

```bash
# in the reference checkout (COOL_EVA_DIR, default ../cool-eva)
node --experimental-strip-types scripts/extract-vcu-tables.ts /path/to/definitions.bin
# then in emcool
node tools/generate-vcu-tables.mts "${COOL_EVA_DIR:-../cool-eva}"
```

## How it runs

Everything that transmits runs in the diagnostics task: the OBD poller and service mode share one event loop. The decode task forwards it the diagnostic replies, 0x7E0–0x7EF and 0x790.

The Bluetooth task hands it the dashboard's requests and collects each answer when it's ready. So a probe that takes seconds never holds up the live stream, and a slow card never holds up decoding.

The loop keeps Node's ordering rules, which is what lets the service code run as C++20 coroutines that match the TypeScript reference step for step:
- Timers fire in due order, and in the order they were set when due together.
- A delay under 1 ms is 1 ms.
- Microtasks run after each timer or frame.

## Writing to the VCU

A separate engine, behind `/vcu-write`. It is the first thing in emcool that can **change** the motorcycle, so it is a separate switch and a separate door from the reads.

### Off by default — and this one stays off even where the others wouldn't

```
emcool> set OBD_ENABLED 1             the bus must be active
emcool> set SERVICE_MODE_ENABLED 1
emcool> set SERVICE_WRITE_ENABLED 1
emcool> restart
```

Every emcool transmitting setting defaults to `0`. `SERVICE_WRITE_ENABLED` is the one that would still be off even if they didn't: it is what lets the phone change a calibration EEPROM, stamp the service record, or clear the fault memory. With it on, the boot journal says so, loudly, at `WARN`.

The POST wants `X-Emcool: service-write` — **deliberately not** the read path's `service-mode`. Neither value is a secret; making them different means a caller that only knows about reads cannot reach a write by accident. `GET /vcu-write` needs no header: it returns the allowlist, the gate, whether the bike's clock is fit to copy, and the last few journal lines, and touches nothing.

### What it can do

Each action is one row in the request parser, and several carry a `confirm=` token so `curl` must state its intent (the two-tap dashboard sends it automatically):

- **`parameter` / `bit` / `parameters`** — write an allowlisted calibration parameter, toggle one named bit of a config word, or write a short batch in one authenticated session. Every write is a compare-and-swap against a fresh read and is read back afterwards.
- **`read-service-stamp` / `set-service-point`** — read, or ⚠️ irreversibly stamp, the A8's last-service block (`31 FC`).
- **`sync-clock`** — broadcast this device's UTC on `0x120`, refused unless its clock is trustworthy and the confirmed minute is still current.
- **`clear-dtcs`** — ⚠️ irreversibly erase the stored codes and freeze frame (OBD Mode 04), parking the poller and reading PID 31 before and after to **prove** the erasure rather than trusting the `44`.
- **`charge-current` / `charge-stop`** — command the charge current on `0x121`, or stop the charge on `0x120`. Exempt from the bike-state gate (a charging operation, and the gate's charge evidence flaps with the trickle); their precondition is a live settled session. Fire-and-forget event frames with no reply, so "sent" is the strongest claim; the ack-watch reads the bike's answer over the next seconds and the GET status carries the verdict.
- **`charge-soc-limit` / `charge-soc-limit-read`** — set or read the "stop charging at N %" limit on `0x2C`; the one action with a real read-back off the VCU's own store.
- **`reset-vcu`** — restart both micros (`11 02`), refused mid-charge.

The **automatic** charge-current controller is a separate `CHARGE_AUTO_ENABLED` stage. See [Automatic charging](#automatic-charging).

### The five locks, in order

1. **`SERVICE_WRITE_ENABLED`** — its own switch, the only one off by default on purpose.
2. **The bus lease** — one thing at a time; a sweep and a write never share the bus.
3. **The safety gate** — the same one the reads use (one opinion, not two). It permits stationary-and-charging, but every other check still applies.
4. **The table-type gate** — only the two parameter actions; it refuses a write unless both micros named a carried table *and* the allowlist's names mean there what they mean here.
5. **The allowlist and ranges** — in the pure layer, re-checked in the codec immediately before the bytes. An identifier not on the list is unexpressible; the HTTP layer takes a name, never a number.

Behind all five, per action: a read of the current value, a compare-and-swap against what the caller believed, and a read-back. The security key is the VCU family's own algorithm (bit-swap then `+0xC1A0BABE`), computed from the seed and never supplied by a caller.

Every attempt — refused, accepted, read-back-mismatch, all of it — is appended to `service-writes.jsonl` on the card, and the last twelve lines ride on the GET status. The reader recovers records a power cut tore.

## Automatic charging

With `CHARGE_AUTO_ENABLED 1` (and `SERVICE_WRITE_ENABLED 1`, since it commands writes), a controller runs that commands the DC charge current on its own during a fast charge. `/charge-auto` controls it.

```
emcool> set OBD_ENABLED 1
emcool> set SERVICE_MODE_ENABLED 1
emcool> set SERVICE_WRITE_ENABLED 1
emcool> set CHARGE_AUTO_ENABLED 1
emcool> restart
```

It is the most autonomous feature here — it actuates the bike with nobody pressing anything — so it sits behind two switches and commands only through the same gated write engine as a manual charge current.

- **The control law** aims the pack at 54 °C and never crosses 55 °C. Every 60 s it estimates the heating rate from a 10-minute window of pack temperatures (a least-squares slope, bounded by how long the reading has been silent), predicts the temperature 12 minutes out, and steps the current up when there is headroom or down when the pack is closing on the target — in clamped amps, never below a 35 A floor. When it has no trend yet it holds, or eases down blindly if it arrived hot. When the pack's own SoC taper will take the current away before heat could, it holds rather than fighting it.
- **It stands down for the rider.** A charge current set by hand — seen as a `dc_charge_limit_selected_a` that is neither the ceiling nor the value the controller last sent — stops it for the rest of that charge. So does any manual `/vcu-write` charge command. A new charge session (a change in `charge_manager_state`) forgets the standdown and the history and starts fresh.
- **`/charge-auto`** turns it on or off (`POST ?mode=automatic|off`, behind `X-Emcool: charge-auto`) and reports, on GET, what it is doing and the plain-English reason — which no header needs.

The decision is a pure function (`decideChargeCurrent`); the controller around it is the stateful shell. ⚠️ A fused multiply-add would change the last bit of the least-squares slope and break parity with the reference implementation's JavaScript, so the whole `emcool_vcu` component is built with `-ffp-contract=off`.

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **The pure parts.** `tools/generate-golden-vcu.mts` runs the reference implementation on random input, and `host/tests/check_vcu.cpp` requires the same output from the C++:
  - 38,675 extended-addressing ISO-TP frames
  - 7,000 replies through the parameter and multi-frame codecs
  - 4,000 records interpreted
  - 6,000 service-gate verdicts
  - 8,029 read outcomes as rows, each also written out and read back
  - 1,500 TABLE_TYPE reports with their re-named rows
  - 3,274 snapshot diffs
  - 200 table switches

  `tools/generate-golden-diagnostics.mts` and `host/tests/check_diagnostics.cpp` add the freeze-frame and lifetime parts:
  - 5,000 freeze-frame replies and their 18,000 formatted values
  - 1,500 lifetime summaries
  - 2,000 `18` lists and the components a read would ask about
- **Service mode itself.** `tools/generate-golden-service.mts` runs the reference implementation's read runner, sweeps and probes, with its OBD poller alongside, against its simulated VCU micros on a fake clock. Its 24 scenarios cover:
  - complete, cancelled and resumed sweeps
  - an unsafe bike, and stale broadcasts
  - silent micros, and refused, silent and wrong-width indices
  - lapsed sessions, and both sides of the 1.5 s session limit
  - carried, uncarried and split TABLE_TYPEs
  - probes of every bank, including refused ones
  - lifetime and freeze-frame reads against micros holding freeze frames, run through the reference's HTTP handlers, and the files they leave
  - the expected-fault list

  `host/tests/check_service.cpp` replays each through the C++. Every millisecond it must send the same frames, log the same lines, and give the same answers to `/vcu-read`, `/vcu-params`, `/vcu-backup` and probes. The checks run clean under AddressSanitizer too.
- **The test can fail.** It catches each of these mutations:
  - the session idle limit 100 ms either way
  - the pace between requests 1 ms short
  - no session retry
  - the gate watchdog at 250 ms
  - the tally's micro order
  - the rule for replacing the latest snapshot
  - a resumed row's stamp
  - an unreleased hold
  - one attempt per freeze-frame component instead of two
  - the lifetime file replaced by a read that answered only as much
  - padding read as a listed component
- **The write path's pure parts.** `tools/generate-golden-write.mts` runs the reference implementation's write code on random input, and `host/tests/check_write.cpp` requires the same output:
  - 4,000 security keys (seeds with the sign bit set included — a wrong key spends a lockout attempt)
  - 10,000 security/write/routine/clear-DTC replies decoded
  - 3,000 service stamps, 3,000 clock verdicts, 3,000 RTC-sync frames both directions
  - the allowlist and 5,500 plan/bit-plan outcomes over 269 targets, in and out of range
  - 2,000 table-gate verdicts (confirmed, split, mismatched, unread)
  - 3,000 `/vcu-write` request parses and the UTC-minute window
  - 2,000 charge-current command frames and the charge-stop frame
  - 3,000 charge-acknowledge verdicts and their messages (did the command take, was it station-limited, superseded, …)
- **The write session on the wire.** `tools/generate-golden-write-session.mts` drives the reference implementation's `writeParameter`, `readServiceStamp`, `resetVcu`, `clearStoredDtcs`, and `createVcuWriteRunner`'s `charge-current`/`charge-stop`, against a scripted bus on a fake clock, and `host/tests/check_write_session.cpp` replays the identical scripted replies through the C++ `WriteSession`/`VcuWriteRunner`: every frame it transmits, at every millisecond, and the outcome it resolves to, must match — the session open, the compare-and-swap, the seed→key unlock, the `2E` write and read-back, all five write outcome paths, and the charge commands' session predicate, ceiling selection (incl. the AC fallback) and response.
- **The write path's tests can fail.** Mutating the security key (`+1`) turns 4,000 pure keys red and puts the wrong bytes in the `27 02` frame the session check watches; mutating the AC ceiling fallback turns the charge-current frame and message red.
- **The automatic controller.** `tools/generate-golden-charge-auto.mts` runs the reference implementation's `decideChargeCurrent`, `estimateHeatingRate`, `estimateSocRate` and `sessionAheadMinutes` on 18,000 random inputs, and `host/tests/check_charge_auto.cpp` requires the same decision and the same rate to the last bit. `tools/generate-golden-charge-auto-loop.mts` then drives the reference's `startChargeAutomatic` — the stateful controller — through six scripted charges (warming, rider standdown, disabled, session restart, manual standdown, hot arrival) with 60 s ticks on a fake clock, and the check replays each through the C++ `ChargeAutomatic`: the currents it commands and the mode/reason/commanded-amps it reports must match at every step. Mutating the 54 °C target turns 81 decisions red.
- **The phone's side.** `tools/check-ble.mts` checks over the real protocol:
  - service requests answered later, as the diagnostics task answers them
  - the service-mode header check, and the separate `service-write` header: `/vcu-write` refuses a POST with no header, and with the *read* header, and reaches the desk only with its own — for `read-service-stamp` and for `charge-current`
  - `/vcu-params` and `/vcu-backup` with no snapshot yet

**Not verified yet** (nothing has run on the ESP32):

- **Against the real VCU.** The requests are byte-identical to the reference implementation's, and its simulated micro models what's been captured from the bike. ⚠️ No write has gone to a real bike from emcool; the write path is proven only against the reference implementation and scripted buses.
- **The card under a sweep.** Rows written while the ride log is writing, and how long `writeSnapshot()` takes on a real card.
