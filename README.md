# emcool

[cool-eva](https://github.com/andy-siu/cool-eva) ported to an ESP32: telemetry for a watercooled Energica Eva Ribelle, read straight off the bike's CAN bus. cool-eva is TypeScript on a Raspberry Pi; emcool is C++ on ESP-IDF.

## Status

**Fifth milestone.** The firmware does the following, and the host checks below pass:

- listens to the CAN bus in listen-only mode, unless you turn on a feature that transmits
- decodes every frame with a port of cool-eva's decoders into live state
- with `OBD_ENABLED` on, polls OBD-II and reads the stored trouble codes, as cool-eva does
- with `SERVICE_MODE_ENABLED` on too, reads the VCU's calibration parameters, the lifetime battery statistics and the stored freeze frames from the dashboard's service mode, while the bike is parked
- with `SERVICE_WRITE_ENABLED` on as well, writes allowlisted VCU parameters, sets the service point, syncs the bike's clock, clears trouble codes, commands or stops the charge current, sets the charge limit and resets the VCU — the one switch that lets a web page change the bike, off by default
- with `CHARGE_AUTO_ENABLED` on too, runs the automatic charge-current controller: it watches the pack temperature during a DC fast charge and eases the current to hold the pack near 54 °C, commanding on a 60 s timer
- records the bike's active faults as the hub mirrors them onto CAN
- steps its clock to GPS time behind cool-eva's corroboration gate
- logs every change to the microSD card, deleting the oldest logs when it fills
- serves its phone dashboard (in `dashboard/`) over Bluetooth, including the Faults tab's code tables and stored codes
- prints status lines to the serial console every 10 s

None of it has run on the board yet: the card, radio and transmit code is compiled, not exercised.

Not yet ported:

- **The rest:** ride log download, fan control, the coolant probes, and the Energica Connectivity Hub link.

## Hardware

**Board:** Copperhill / SK Pang ESP32 CAN-BUS board, rev B ([schematic](https://copperhilltech.com/content/ESP32_CAN_rev_B.pdf)).

- ESP32-WROOM-32: 520 KB SRAM, 4 MB flash, no PSRAM.
- MCP2562 CAN transceiver, with TXD on GPIO25 and RXD on GPIO26. Those are the firmware defaults.
- A CP2104 USB-C serial bridge, for `idf.py flash monitor`.

**J3**, the CAN and power connector:

| Pin | Signal |
| --- | --- |
| 1 | CAN_L |
| 2 | CAN_H |
| 3 | GND |
| 4 | +12V in |

Pin 1 is CAN_**L**: the schematic crosses the two lines before the connector.

### Before connecting it to the bike

- **Take the terminator out.** The board's 120 Ω terminator, R4, is switched in by jumper JP1. The bike's bus is already terminated at both ends, and a third resistor at the OBD port drops it from 60 Ω to 40 Ω. Remove the JP1 jumper.
  - To check, measure between CAN_H and CAN_L on J3 with the board unpowered and disconnected. About 120 Ω means the terminator is still in; open circuit means it is out.
- **Power it over USB-C rather than J3's +12V.** J3 pin 4 feeds an NCV4264-2 5 V regulator, which onsemi rates at 100 mA. The 3.3 V regulator, an MIC5504 rated at 300 mA, runs off that same 5 V rail, so everything on the board would share 100 mA. Bluetooth alone needs more than that: the ESP32 datasheet (v5.3, Table 5-4) gives 130 mA typical to transmit and 95–100 mA to receive.
  - Feed USB-C from a 12 V → 5 V USB adapter instead, and leave J3 pin 4 unconnected.
  - With USB power, J3 only needs pins 1–3.

### Pins for add-on modules

The microSD pins are firmware defaults (`idf.py menuconfig` → emcool → microSD card). The coolant-probe pins are planned only, not coded yet.

| Function | GPIO | Header pin |
| --- | --- | --- |
| microSD SCK | 5 | JP2-7 (SCK) |
| microSD MOSI | 18 | JP2-8 (MOSI) |
| microSD MISO | 19 | JP2-9 (MISO) |
| microSD CS | 17 | JP2-6 |
| MAX31865 SCK | 14 | JP2-3 |
| MAX31865 MOSI | 13 | JP3-4 |
| MAX31865 MISO | 27 | JP3-6 |
| MAX31865 CS, coolant in | 32 | JP3-8 |
| MAX31865 CS, coolant out | 33 | JP3-7 |

Power for these modules:

- **3.3 V:** JP2-1 or JP3-2.
- **+5 V:** JP3-1.
- **GND:** JP2-2 or JP3-3.

Other pins:

- **Don't use GPIO12 (JP3-5).** It is the strapping pin that selects the flash voltage, so anything that pulls it high at reset, such as an SD card's MISO pull-up, stops the WROOM-32 from booting.
- **Still free** for the fan and anything else:
  - GPIO16 and GPIO21.
  - GPIO22 and GPIO23, which have 10 kΩ pull-ups for the QWIIC connector.
  - Input-only pins GPIO34–39.
- **The RGB LED** is on GPIO2 (red), GPIO15 (green) and GPIO4 (blue).

## Build and flash

Needs ESP-IDF v6.1.

```bash
. ~/esp/esp-idf/export.sh
idf.py build flash monitor
```

The CAN and microSD pins default to this board's (GPIO25/26, and the SPI header). For a different board, change them under `idf.py menuconfig` → emcool. Setting a pin to -1 turns that part off.

## Settings

Per-bike settings live in the ESP32's NVS, emcool's counterpart of cool-eva's `/etc/default/cool-eva`. They survive reflashing and firmware updates; only `idf.py erase-flash` clears them. Set them from the serial console that `idf.py monitor` opens. [`docs/setup.md`](docs/setup.md) is the complete reference — every console command and every setting with its type, default, range and dependencies; the summary here covers the common ones.

```
emcool> show
emcool> set BLE_SECRET "a phrase you can type on a phone"
emcool> set CUSTOM_BMS_CONFIG 1
emcool> restart
```

| Setting | Default | What it does |
| --- | --- | --- |
| `BLE_SECRET` | not set | The phone dashboard's link secret, as cool-eva's `BLE_SECRET`: 12–128 characters. Unset keeps Bluetooth off. |
| `BLE_NAME` | `emcool` | The name in the phone's device list. |
| `CUSTOM_BMS_CONFIG` | `0` | `1` if the BMS runs the custom LiBAL config, as cool-eva's `CUSTOM_BMS_CONFIG`. |
| `OBD_ENABLED` | `0` | `1` to poll OBD-II and read trouble codes, as cool-eva's `OBD_ENABLED`. This puts the CAN controller on the bus as an active node. See [Transmitting](#transmitting). |
| `ELOCK_ENABLED` | `0` | `1` to read `keys_paired` from the E-LOCK once at startup, as cool-eva's `ELOCK_ENABLED`. Needs `OBD_ENABLED`. |
| `SERVICE_MODE_ENABLED` | `0` | `1` to let the dashboard read the VCU's calibration parameters, as cool-eva's `SERVICE_MODE_ENABLED`. Only while the bike is parked and out of drive. Needs `OBD_ENABLED`. See [`docs/service-mode.md`](docs/service-mode.md). |
| `SERVICE_WRITE_ENABLED` | `0` | `1` to let the dashboard **write** to the VCU (`/vcu-write`), as cool-eva's `SERVICE_WRITE_ENABLED`. The one switch that lets a web page change the bike, so off by default even though it is a transmitting setting like the rest. Needs `OBD_ENABLED` and `SERVICE_MODE_ENABLED`. See [`docs/service-mode.md`](docs/service-mode.md). |
| `CHARGE_AUTO_ENABLED` | `0` | `1` for the automatic charge-current controller, as cool-eva's `CHARGE_AUTO_ENABLED`. It commands the DC charge current on a 60 s timer to hold the pack near 54 °C. Needs `SERVICE_WRITE_ENABLED` to actuate. See [`docs/service-mode.md`](docs/service-mode.md). |
| `STATUS_REPORT` | `1` | Whether the console prints the periodic status report. Applies at once. |
| `STATUS_INTERVAL_S` | `10` | Seconds between status reports, 5–3600. Applies at once. |

How the console behaves:

- **Restart to apply.** Settings are read at boot, so a change takes effect at `restart`. `show` marks any change still waiting for one. The two status settings are the exception and apply at once.
- **The secret is never printed back.** `show` reports only whether it's set and its length. Quote values that contain spaces.
- **Names ignore case.** Flags take `1`/`0`, `on`/`off`, `true`/`false` or `yes`/`no`.

**What `CUSTOM_BMS_CONFIG` does.** It tells emcool which BMS config to expect when it works out which frame carries the true pack temperature (`batt_temp_lo`/`batt_temp_hi`), ported from cool-eva's `src/can/pack-temperature.ts`. The frames still win.

- **Set to 1 on a custom-config pack:** the true temperatures come from the long 0x660 frame from the first one.
- **Left at 0 on a custom-config pack:** the routing still switches to 0x660 when the first long one arrives, and logs an error saying to set it. But if 0x200 was already flowing for 5 s by then, a few lowered readings will have been logged as the true temperature first.
- **Set to 1 on a stock pack:** the true temperatures are never logged, and a warning says so.

The values loaded at boot are in the first lines of the console output. cool-eva's other environment variables come back with the features they configure.

### Transmitting

emcool sends nothing on the bike's bus unless a setting asks it to, and every such setting defaults to `0`. Some of cool-eva's, such as `OBD_ENABLED`, default to on.

With every transmitting setting at `0`, the CAN controller is listen-only: it doesn't even acknowledge frames. Setting `OBD_ENABLED 1` and restarting makes it an active node, as cool-eva's `can0` is. It acknowledges frames, polls mode 01 PIDs at 2 Hz, and reads the stored trouble codes about once a minute.

What goes on the bus, what gets recorded, and what has been verified are in [`docs/obd.md`](docs/obd.md).

With `SERVICE_MODE_ENABLED 1` as well, the dashboard's service mode can sweep the VCU's calibration parameters, or probe one, while the bike is safe to service. It only reads. The snapshots it keeps on the card are described in [`docs/service-mode.md`](docs/service-mode.md).

With `SERVICE_WRITE_ENABLED 1` on top of those, the dashboard can also **change** the bike: write an allowlisted calibration parameter, set the service point, sync the bike's clock, clear the stored trouble codes, command or stop the charge current, set the charge limit, or reset the VCU. This is the only setting that lets a web page change the motorcycle, so it is off by default and loud in the journal when on — unlike cool-eva, every emcool transmitting setting is off by default, and this one would be off even if they were not. Every write goes behind the same safety gate as the reads, behind its own `X-Emcool: service-write` header (deliberately not the read path's `service-mode`), and is journalled to `service-writes.jsonl`. The allowlist, the five gates and the per-action confirmations are in [`docs/service-mode.md`](docs/service-mode.md).

With `CHARGE_AUTO_ENABLED 1` as well, the automatic charge-current controller runs: during a DC fast charge it watches the pack temperature and, on a 60 s timer, eases the current up or down to hold the pack near 54 °C without crossing 55 °C — the same thermal control law as cool-eva's, commanding through the write engine above (so it needs `SERVICE_WRITE_ENABLED` to actuate, and stands down the moment a current is set by hand). `/charge-auto` turns it on or off and shows what it is doing and why. It is the most autonomous feature here; see [`docs/service-mode.md`](docs/service-mode.md).

### The status report

Every 10 s the console prints five lines, emcool's stand-in for watching cool-eva's journal:

- **can:** frames per second, drops, bus state, and free memory now and at its lowest
- **ride log:** whether the card is mounted, the boot and part being written, the write rate, losses and rotations, and the clock's trust
- **bluetooth:** advertising or connected, the packet size, and notifications per second
- **obd:** off and listen-only, or the poller's round, frames sent and any never acknowledged, the stored codes' state, and whether something holds the poller
- **live:** a few current values, without position

It prints over whatever you're typing, so it can be paused or slowed:

```
emcool> status            prints a report now, and says how often it reports
emcool> status off        stops the periodic report
emcool> status on         starts it again
emcool> status every 30   reports every 30 s (5–3600) and turns it on
```

These apply at once and are saved as `STATUS_REPORT` and `STATUS_INTERVAL_S`, so a report you've turned off stays off after a restart.

## Ride log

Readings go to `RIDES/<boot>/PART<n>.LOG` on a FAT32 microSD card, in cool-eva's segment format as plain text, minus location. When the card fills, the oldest parts are deleted to make room, first in, first out. Nothing else on the card is ever touched. The format, what a power cut costs and what has been verified are in [`docs/ride-log-format.md`](docs/ride-log-format.md). That doc also covers preparing a card: FAT32, and the firmware never formats one.

To import the card into the same `rides.db` cool-eva's Grafana dashboards read:

```bash
node tools/import-ride-log.mts /Volumes/EMCOOL/RIDES --out rides.db
```

This runs through cool-eva's own `src/db.ts`, so it needs a cool-eva checkout with its dependencies installed (`../cool-eva`, or set `COOL_EVA_DIR`).

## Phone dashboard (Bluetooth)

emcool ships its own phone dashboard in [`dashboard/`](dashboard/) — a static site copied from cool-eva's and rebranded, paired with emcool's wire protocol (the handshake salts and the request header were renamed from cool-eva's, so it talks to this firmware, not cool-eva's). Host it over HTTPS and open it in Bluefy on iOS or Chrome on Android. A [GitHub Pages workflow](.github/workflows/pages.yml) publishes it — see [`docs/github-pages.md`](docs/github-pages.md), [`dashboard/README.md`](dashboard/README.md) and [`docs/ble.md`](docs/ble.md).

1. Set `BLE_SECRET` from the serial console (see [Settings](#settings)) and `restart`.
2. Enter the same secret in the dashboard's menu under *Bluetooth*.
   - The live tiles, the status sheet and the Faults tab work. The Faults tab shows stored codes only with `OBD_ENABLED` on.
   - The parameters page works, as do service mode's reads with `SERVICE_MODE_ENABLED` on and its writes with `SERVICE_WRITE_ENABLED` on. The dashboard's other requests answer "not on emcool yet". What differs from cool-eva's server, the memory it takes, and what has been verified are in [`docs/ble.md`](docs/ble.md).

## Checks

```bash
tools/check.sh
```

The checks run on the laptop. They need a cool-eva checkout (`../cool-eva`, or set `COOL_EVA_DIR`), Node 22.6+, CMake and Ninja, but not ESP-IDF.

- **The registry is current.** `components/emcool_signals/src/registry.gen.cpp` is generated from cool-eva's `src/can/registry.ts` by `tools/generate-registry.mts`. Re-run that script when cool-eva's registry changes.
- **The decoders match cool-eva.** `tools/generate-golden.mts` runs about 200,000 synthetic frames through cool-eva's own `decodeFrame()`. `host/tests/check_decoders.cpp` then requires the C++ to produce the same keys, in the same order, with bit-identical values, and to warn exactly as often.
  - The corpus covers every 11-bit id at every length, biased payloads that get past each decoder's gates, and GPS sub-frame sequences.
  - It fails if any decoder key never appears.
- **No location reaches the ride log.** The decoded corpus goes through the signal store, and the check fails if any signal in `kExcludedFromRideLog` is written.
- **The pack-temperature routing matches cool-eva.** `tools/generate-golden-pack-temperature.mts` drives cool-eva's own routing on a scripted clock, through 419 runs of 0x200/0x660 sequences:
  - stock and custom configs, with the setting right and wrong
  - short, long and late 0x660 frames
  - echo and clamp faults

  `host/tests/check_pack_temperature.cpp` requires the same values on every frame and the same number of errors, warnings and info lines in every run.
- **The OBD side matches cool-eva, to the millisecond.** `tools/generate-diagnostic-tables.mts` generates Energica's code table and the two static Faults bodies from cool-eva's own modules, and `--check` fails if they're stale.
  - `tools/generate-golden-obd.mts` runs cool-eva's own poller, trouble-code reads, hold and E-LOCK read against a scripted ECU on a fake clock: 72 scenarios over four simulated hours.
  - `host/tests/check_obd.cpp` replays each scenario through the C++. Every millisecond it must send the same frames, record the same signals, log the same lines, and serve the same `/stored-dtcs` body.
  - It also checks the pure parts exhaustively or by fuzzing: all 65,536 codes, every NRC, and 94,000 ISO-TP frames.
  - Details, and the mutations it catches, are in [`docs/obd.md`](docs/obd.md).
- **Service mode matches cool-eva, to the millisecond.** `tools/generate-vcu-tables.mts` generates the 29 parameter tables from cool-eva's catalogue, and `--check` fails if they're stale.
  - `tools/generate-golden-vcu.mts` and `host/tests/check_vcu.cpp` check the pure parts on random input: codecs, the service gate, rows, the TABLE_TYPE report and diffs.
  - `tools/generate-golden-service.mts` drives cool-eva's read runner, sweeps and probes, with the OBD poller alongside, against cool-eva's simulated VCU micros.
  - `host/tests/check_service.cpp` replays each scenario through the C++. Every millisecond must match.
  - Details are in [`docs/service-mode.md`](docs/service-mode.md).
- **Numbers and ordering behave as in JavaScript.** `host/tests/check_text.cpp` checks 200,000 doubles are written as Node's `String()` writes them, and `host/tests/check_async.cpp` checks the event loop keeps Node's order.
- **The settings console behaves.** `host/tests/check_settings.cpp` checks validation, that the secret is never printed, flag spellings, NVS key lengths, restart notices, and that a stored value which no longer validates falls back to its default. It also checks the `status` commands and the report's schedule: off, on, a changed interval, and an on-demand report that leaves the cadence alone.
- **The signal store follows cool-eva's rules.** `host/tests/check_signals.cpp` checks the deadband gate, non-finite readings, unknown keys and JavaScript-compatible rounding.
- **The clock gate matches cool-eva.** `tools/generate-golden-clock.mts` drives cool-eva's `GpsClockGate` through 88,000 randomized offers, and `host/tests/check_clock.cpp` requires the same verdict every time.
  - The scenarios include cold boots at 1970, drift, outliers, week-number rollovers and contested anchors.
- **The ride log round-trips.** `host/tests/check_ride_log.cpp` checks the sealing rules and the writer's files. It also runs the decoder corpus through the whole C++ pipeline onto disk.
  - `tools/check-ride-log.mts` then reads it back exactly, simulates power cuts and card damage, and imports it into `rides.db` through cool-eva's `db.ts`.
  - It runs again on a full card, simulated and then as a real FAT image on macOS. Exactly the newest readings must survive.
- **The Bluetooth protocol works piece by piece.** `host/tests/check_ble_wire.cpp` checks the crypto against published vectors and the deflate encoder against zlib. It also covers the JSON reader on hostile input, framing, and the handshake, including a wrong secret, the cooldown and replay.
- **The phone's own code talks to the server.** `tools/check-ble.mts` drives the C++ server, built for the Mac, with cool-eva's `ble-wire.js`, the code the phone runs.
  - It checks the handshake.
  - It requires every snapshot to equal live state exactly, at MTU 185 and at MTU 23.
  - It covers patches, `/status`, tampering, replay, unsubscribe and disconnect.
  - `/dtc-table` and `/fault-infokeys` must match cool-eva's bodies byte for byte, and `/stored-dtcs` must match what cool-eva's own code serves for the same reply.
  - The host build needs OpenSSL 3, which Homebrew provides.

## Layout

| Path | What |
| --- | --- |
| `components/emcool_can` | Frame decoders and the pack-temperature routing, ported from cool-eva `src/can/`. Pure, and builds on the host too. |
| `components/emcool_signals` | Signal registry (generated), live state, the deadband gate, and the ride-log exclusions. |
| `components/emcool_clock` | The GPS clock gate and clock trust, ported from cool-eva `src/gps/clock*.ts`. |
| `components/emcool_ride_log` | Ride-log segments, the on-card record format and the file writer. Plain POSIX calls, so the host runs the same code. |
| `components/emcool_ble_wire` | The phone link's wire protocol and session, ported from cool-eva `public/lib/ble-wire.js` and `src/ble-server/session.ts`. Crypto is mbedTLS PSA on the device and OpenSSL on the host. |
| `components/emcool_ble_server` | The phone link's server: live stream, `/status`, request handling. Ported from cool-eva `src/ble-server/`; transport-free. |
| `components/emcool_obd` | OBD-II: the PID poller, trouble-code reads over ISO-TP, the poller's hold, the E-LOCK read, and Energica's code table (generated). Ported from cool-eva `src/can/obd*.ts`, `elock.ts`, `iso-tp.ts` and `src/diagnostics/`; transport-free. |
| `components/emcool_vcu` | Service mode: the KWP2000 client, sweeps and probes, the service gate, the 29 parameter tables (generated), the snapshots on the card, the write engine (allowlist, table gate, write session, service actions, clear-DTCs, charge current/stop/SOC-limit, audit journal) and the automatic charge-current controller. Ported from cool-eva `src/vcu/` and `src/charge/`; transport-free. |
| `components/emcool_async` | An event loop with Node's ordering rules, and C++20 coroutines on it, for porting cool-eva's async code. |
| `components/emcool_text` | Allocation-free text and JSON building, a window at a time for bodies bigger than any buffer. |
| `components/emcool_settings` | The per-bike settings and the console commands that change them; NVS on the device. |
| `components/emcool_log` | Logging through ESP_LOG on the device and stderr on the host. |
| `main` | Firmware: CAN receive and transmit, the decode, diagnostics, ride-log and Bluetooth tasks, the microSD mount, NimBLE, NVS settings and the serial console, the clock step and the status report. |
| `host` | Host build of the components, plus the checks. |
| `tools` | Generators, the ride-log reader and import, and `check.sh`. |
| `docs` | Design notes: `ride-log-format.md`, `ble.md`, `obd.md`, `service-mode.md`. |

## Decisions

- **The ride log is plain text on a FAT32 microSD card**, unlike cool-eva's encrypted log. Position, heading and altitude are never written to it; the list is in `components/emcool_signals/include/emcool/signals/ride_log_policy.hpp`.
- **Bluetooth only, no Wi-Fi.** The phone dashboard stays cool-eva's, loaded from the iOS app or GitHub Pages. The Bluetooth protocol is byte-compatible with cool-eva's `public/lib/ble-wire.js`, and `tools/check-ble.mts` holds it to that.
- **Nothing goes on the bus unless a setting asks for it.** Every transmitting feature defaults to off, even where cool-eva's defaults to on. With all of them off, the CAN controller is listen-only.

## License

AGPL-3.0, the same as cool-eva, because the decoders and the generated registry are a port of cool-eva's code. See `LICENSE`.
