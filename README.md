# emcool

Telemetry for an Energica electric motorcycle, on an ESP32 plugged into its CAN bus. emcool decodes every frame on the bus into live values, logs them to a microSD card, and serves a phone dashboard over Bluetooth. With the settings for them turned on, it also polls OBD-II, reads and writes the VCU's parameters in service mode, controls the DC charge current to keep the pack cool, and serves its console and ride log over Wi-Fi. It's C++ on ESP-IDF.

## Status

**Every planned stage is built**, and the laptop checks below pass. The firmware:

- **Reads the bus** in listen-only mode: it doesn't even acknowledge frames unless a setting asks it to transmit.
- **Decodes every frame** into live values: over 500 named signals.
- **Steps its clock to GPS time**, once five GPS readings agree with each other and with the last time it trusted.
- **Logs every change to the microSD card**, deleting the oldest logs when the card fills.
- **Records the bike's active faults** as the Energica Connectivity Hub mirrors them onto the bus.
- **Serves the phone dashboard over Bluetooth**: live tiles, faults, parameters, service mode, and downloading the ride log.
- **Prints a status report** to the serial console every 10 s.
- With **`OBD_ENABLED`**, polls OBD-II and reads the stored trouble codes.
- With **`SERVICE_MODE_ENABLED`** too, reads the VCU's calibration parameters, the lifetime battery statistics and the stored freeze frames, while the bike is parked.
- With **`SERVICE_WRITE_ENABLED`** as well, writes allowlisted VCU parameters, sets the service point, syncs the bike's clock, clears trouble codes, commands or stops the charge current, sets the charge limit and resets the VCU. This is the one switch that lets a web page change the bike, so it's off by default.
- With **`CHARGE_AUTO_ENABLED`** on top, runs the automatic charge-current controller. During a DC fast charge it eases the current to hold the pack near 54 °C, on a 60 s timer.
- With **`WIFI_SSID`** and **`ADMIN_PASSWORD`**, joins that Wi-Fi network. It serves the console on port 23 and the ride log as a web page on port 80.

**Not built:** fan control, the coolant-temperature probes (pins are reserved below), and a direct link to the Connectivity Hub.

**Not yet run on the board.** Everything is compiled, and the laptop checks drive most of it, but the CAN controller, the card, Bluetooth and Wi-Fi haven't run on real hardware.

## Hardware

**Board:** Copperhill / SK Pang ESP32 CAN-BUS board, rev B ([schematic](https://copperhilltech.com/content/ESP32_CAN_rev_B.pdf)).

- ESP32-WROOM-32: 520 KB SRAM, 4 MB flash, no PSRAM. Bluetooth 4.2 and 2.4 GHz Wi-Fi.
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
- **Power it over USB-C rather than J3's +12V.** J3 pin 4 feeds an NCV4264-2 5 V regulator, which onsemi rates at 100 mA. The 3.3 V regulator, an MIC5504 rated at 300 mA, runs off that same 5 V rail, so everything on the board would share 100 mA. Bluetooth alone needs more than that: the ESP32 datasheet (v5.3, Table 5-4) gives 130 mA typical to transmit and 95–100 mA to receive. Wi-Fi needs more again.
  - Feed USB-C from a 12 V → 5 V USB adapter instead, and leave J3 pin 4 unconnected.
  - With USB power, J3 only needs pins 1–3.

### Pins for add-on modules

The microSD pins are firmware defaults (`idf.py menuconfig` → emcool → microSD card); [`docs/sd-card-wiring.md`](docs/sd-card-wiring.md) has the card's pinout and the pull-ups it needs. The coolant-probe pins are planned only, not coded yet.

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

- **Pins:** the CAN and microSD pins default to this board's (GPIO25/26, and the SPI header). For a different board, change them under `idf.py menuconfig` → emcool. Setting a pin to -1 turns that part off.
- **Build options:** these live in `sdkconfig.defaults`. The generated `sdkconfig` isn't tracked, so after changing the defaults, delete `sdkconfig` and the build regenerates it.
- **Optimised for size:** the firmware is built with `-Os`, not `-Og`, because Bluetooth and Wi-Fi together don't fit otherwise. It's 1.81 MB of the 1.875 MB app slot.

### Updating without the cable

After the first USB flash, new firmware can go on over the air, from the microSD card or over Wi-Fi. Both write the new image into the spare app slot while the bike keeps running, check it, and boot it at the next `restart`:

```
emcool> update /emcool.bin      after copying build/emcool.bin onto the card
emcool> restart
```

```bash
curl -u :'the admin password' -T build/emcool.bin http://emcool.local/update
```

- **On trial:** a new image is kept once it has run 30 s. A crash, a watchdog reset, a power cut or a `restart` before then boots the old one again, and `update status` says so. So let it run 30 s before restarting it.
- **Parked, with the bus free:** writing flash pauses the CAN bus's interrupt for tens of milliseconds at a time, which drops frames and could cut a service write short. So an update waits for what `restart` waits for: it's refused while a fresh reading says the bike is moving or in drive, and while service mode or the OBD poller's hold has the bus.
- **Refused, with the slot untouched:** a file that isn't an ESP32 app, another project's firmware, the bootloader or a merged image. Anything cut short or corrupt is caught by the image's own hash before the boot slot changes.
- **The first time needs the cable.** Trial boots are a bootloader setting (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), and only `idf.py flash` writes the bootloader. A board flashed before this has no trial boot: an update still works, but nothing undoes a bad one.
- **USB is always the way back.** `idf.py flash` writes slot 0 and points the boot at it, whatever the update did.

Each slot holds a whole image, so the app has to stay under 1.875 MB.

## Settings

Per-bike settings live in the ESP32's NVS. They survive reflashing and firmware updates; only `idf.py erase-flash` clears them. Set them from the serial console that `idf.py monitor` opens, or from the dashboard's **Settings → Bike console**, which runs the same commands over Bluetooth. [`docs/setup.md`](docs/setup.md) is the complete reference — every console command and every setting with its type, default, range and dependencies; the summary here covers the common ones.

```
emcool> show
emcool> set BLE_SECRET <your key>      Bluetooth turns on at once
emcool> set CUSTOM_BMS_CONFIG 1
emcool> restart
```

| Setting | Default | What it does |
| --- | --- | --- |
| `BLE_SECRET` | not set | The phone dashboard's Bluetooth key: 8-64 characters with at least one letter and one digit, or 64 hex characters (`openssl rand -hex 32` makes one). Setting it turns Bluetooth on at once; unset keeps it off. |
| `BLE_NAME` | `emcool` | The name in the phone's device list. |
| `CUSTOM_BMS_CONFIG` | `0` | `1` if the BMS runs the custom LiBAL config. |
| `OBD_ENABLED` | `0` | `1` to poll OBD-II and read trouble codes. This puts the CAN controller on the bus as an active node. See [Transmitting](#transmitting). |
| `ELOCK_ENABLED` | `0` | `1` to read `keys_paired` from the E-LOCK once at startup. Needs `OBD_ENABLED`. |
| `SERVICE_MODE_ENABLED` | `0` | `1` to let the dashboard read the VCU's calibration parameters, only while the bike is parked and out of drive. Needs `OBD_ENABLED`. See [`docs/service-mode.md`](docs/service-mode.md). |
| `SERVICE_WRITE_ENABLED` | `0` | `1` to let the dashboard **write** to the VCU (`/vcu-write`). The one switch that lets a web page change the bike. Needs `OBD_ENABLED` and `SERVICE_MODE_ENABLED`. See [`docs/service-mode.md`](docs/service-mode.md). |
| `CHARGE_AUTO_ENABLED` | `0` | `1` for the automatic charge-current controller. It commands the DC charge current on a 60 s timer to hold the pack near 54 °C. Needs `SERVICE_WRITE_ENABLED` to actuate. See [`docs/service-mode.md`](docs/service-mode.md). |
| `WIFI_SSID` | not set | The Wi-Fi network to join, 2.4 GHz. With `ADMIN_PASSWORD` set too, the bike serves the console on port 23 and the ride log on port 80. See [Wi-Fi](#wi-fi). |
| `WIFI_PASSWORD` | not set | That network's password, 8–63 characters. Unset for an open network. |
| `ADMIN_PASSWORD` | not set | What the Wi-Fi console and the ride-log page ask for, 12–128 characters. Unset keeps Wi-Fi off. |
| `STATUS_REPORT` | `1` | Whether the console prints the periodic status report. Applies at once. |
| `STATUS_INTERVAL_S` | `10` | Seconds between status reports, 5–3600. Applies at once. |

The console's commands, and its [card commands](docs/setup.md#the-card) (`dir`, `cd`, `type`, `rm`, `cp`, `mv`, …):

| Command | What it does |
| --- | --- |
| `show` | Every setting, its value, and whether a change is waiting for a restart. |
| `set NAME VALUE` | Saves a setting. Quote values with spaces. |
| `unset NAME` | Puts a setting back to its default. |
| `status [on \| off \| every SECONDS]` | Prints a status report now, or changes how often it prints. |
| `network` | The Wi-Fi network joined and its signal, and the address, netmask, gateway and DNS server it handed out. |
| `restart` | Reboots, so saved settings take effect. |

How the console behaves:

- **Restart to apply.** Settings are read at boot, so a change takes effect at `restart`. `show` marks any change still waiting for one. The two status settings are the exception and apply at once.
- **Secrets are never printed back.** `show` reports only whether a secret or password is set, and its length.
- **Names ignore case.** Flags take `1`/`0`, `on`/`off`, `true`/`false` or `yes`/`no`.

**What `CUSTOM_BMS_CONFIG` does.** It tells emcool which BMS config to expect when it works out which frame carries the true pack temperature (`batt_temp_lo`/`batt_temp_hi`). The frames still win.

- **Set to 1 on a custom-config pack:** the true temperatures come from the long 0x660 frame from the first one.
- **Left at 0 on a custom-config pack:** the routing still switches to 0x660 when the first long one arrives, and logs an error saying to set it. But if 0x200 was already flowing for 5 s by then, a few lowered readings will have been logged as the true temperature first.
- **Set to 1 on a stock pack:** the true temperatures are never logged, and a warning says so.

The values loaded at boot are in the first lines of the console output.

If a setting makes the board crash at boot, three crashes in a row within 30 s of boot start it in **safe mode**: only the serial console runs, so you can undo the setting and `restart`. See [`docs/setup.md`](docs/setup.md#when-a-setting-runs-the-memory-out).

### Transmitting

emcool sends nothing on the bike's bus unless a setting asks it to, and every such setting defaults to `0`.

With every transmitting setting at `0`, the CAN controller is listen-only: it doesn't even acknowledge frames. Setting `OBD_ENABLED 1` and restarting makes it an active node. It acknowledges frames, polls mode 01 PIDs at 2 Hz, and reads the stored trouble codes about once a minute. What goes on the bus, what gets recorded, and what has been verified are in [`docs/obd.md`](docs/obd.md).

Each layer needs the one before it:

```
OBD_ENABLED=1                 the CAN controller becomes an active node; polls OBD, reads codes
  └ SERVICE_MODE_ENABLED=1    the dashboard may READ the VCU (parameters, lifetime, freeze frames)
      └ SERVICE_WRITE_ENABLED=1    the dashboard may CHANGE the bike (/vcu-write)  ⚠️ off on purpose
          └ CHARGE_AUTO_ENABLED=1  the controller commands the charge current on its own
```

- **`SERVICE_MODE_ENABLED`:** the dashboard's service mode can sweep the VCU's calibration parameters, or probe one, while the bike is safe to service. It only reads. The snapshots it keeps on the card are described in [`docs/service-mode.md`](docs/service-mode.md).
- **`SERVICE_WRITE_ENABLED`:** the dashboard can also **change** the bike: write an allowlisted calibration parameter, set the service point, sync the bike's clock, clear the stored trouble codes, command or stop the charge current, set the charge limit, or reset the VCU. It's off by default and says so loudly in the log when on. Every write goes behind the same safety gate as the reads, behind its own `X-Emcool: service-write` header (deliberately not the read path's `service-mode`), and is recorded in `service-writes.jsonl`.
- **`CHARGE_AUTO_ENABLED`:** during a DC fast charge, the controller watches the pack temperature and, on a 60 s timer, eases the current up or down to hold the pack near 54 °C without crossing 55 °C. It commands through the write engine, so it needs `SERVICE_WRITE_ENABLED`, and it stands down the moment a current is set by hand. `/charge-auto` turns it on or off and shows what it is doing and why.

The allowlist, the safety gates and the per-action confirmations are in [`docs/service-mode.md`](docs/service-mode.md).

### The status report

Every 10 s the console prints six lines:

- **can:** frames per second, drops, bus state, and free memory now and at its lowest
- **ride log:** whether the card is mounted, the boot and part being written, the write rate, losses and rotations, and the clock's trust
- **bluetooth:** advertising or connected, the packet size, and notifications per second
- **wifi:** off, or the address, the console's and page's use, and reconnects
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

Readings go to `RIDES/<boot>/PART<n>.LOG` on a FAT32 microSD card, as plain text, minus location. When the card fills, the oldest parts are deleted to make room, first in, first out. Nothing else on the card is ever touched. The format, what a power cut costs and what has been verified are in [`docs/ride-log-format.md`](docs/ride-log-format.md). That doc also covers preparing a card: FAT32, and the firmware never formats one.

There are three ways to get it off the bike, and each gives files that import the same way:

- **Take the card out** and read its `RIDES/` folder. This is the way to take everything.
- **Over Bluetooth**, from the dashboard's menu: *Download recent ride log*, the newest 16 MB or so. Bluetooth is slow, so this is for a recent ride. See [`docs/ble.md`](docs/ble.md).
- **Over Wi-Fi**, from `http://<bike>/`: every boot and part, much faster. See [`docs/wifi.md`](docs/wifi.md).

To import into `rides.db`, the SQLite file Grafana reads:

```bash
node tools/import-ride-log.mts /Volumes/EMCOOL/RIDES --out rides.db
node tools/import-ride-log.mts emcool-00012.3-00014.1.log --out rides.db
```

The import needs only Node 22.13 or newer, which has SQLite built in. It reports damaged records, readings stamped before the clock could be trusted, and any records missing from a session.

## Wi-Fi

With `WIFI_SSID` and `ADMIN_PASSWORD` set, the bike joins that network, such as your garage Wi-Fi or a phone hotspot (2.4 GHz only). It serves two things, both behind the admin password:

- **The console** on port 23: `nc <bike> 23`, with the same commands as the USB console. The log follows it.
- **The ride log** at `http://<bike>/`: every boot and part, to download in any browser, or with `curl -u x:'password' -OJ 'http://<bike>/dl?mb=256'`.

`network` on either console, the boot log and the status line all give the address. ⚠️ It's all plain text on the network, the password included, so use it on a network you control. [`docs/wifi.md`](docs/wifi.md) has the details, the costs and what's verified.

## Phone dashboard (Bluetooth)

emcool ships its own phone dashboard in [`dashboard/`](dashboard/), a static web app maintained in this repo and paired with this firmware. It is served at **https://emcool.athegen.com** (GitHub Pages, custom domain; see [`docs/hosting.md`](docs/hosting.md)). Open it in Bluefy on iOS or Chrome on Android. See also [`dashboard/README.md`](dashboard/README.md) and [`docs/ble.md`](docs/ble.md).

1. On the serial console, `set BLE_SECRET <your key>` (see [Settings](#settings)). Bluetooth turns on at once; keep a copy of the key.
2. In the dashboard, open *Settings → Connections*, paste the key under *Enter the bike's Bluetooth key* and tap *Connect this phone*.
   - The live tiles, the status sheet and the Faults tab work. The Faults tab shows stored codes only with `OBD_ENABLED` on.
   - The parameters page works, as do service mode's reads with `SERVICE_MODE_ENABLED` on and its writes with `SERVICE_WRITE_ENABLED` on.
   - The sheet's *Download recent ride log* downloads the newest part of the ride log (see [Ride log](#ride-log)).
   - Settings shows what the bike runs with, read-only; settings change on the serial console.
   - The dashboard's other requests answer "not on emcool yet". The link's security, the memory it takes, and what has been verified are in [`docs/ble.md`](docs/ble.md).

## Checks

```bash
tools/check.sh
```

The checks run on the laptop, not the board. They need Node 22.13+, CMake, Ninja and OpenSSL 3 (Homebrew), but not ESP-IDF.

Most of emcool's logic is checked against a **reference implementation**: an earlier TypeScript version of the same telemetry. Its generators run the reference's own code on scripted input, and the C++ must produce the same output, value for value and often to the millisecond. So the checks also need a checkout of the reference next to this repo (`../cool-eva`, or set `COOL_EVA_DIR`). Nothing on the bike, and nothing in the ride-log import, needs it.

- **The generated tables are current.** The signal registry, Energica's fault-code tables and the 29 VCU parameter tables are generated from the reference by `tools/generate-registry.mts`, `tools/generate-diagnostic-tables.mts` and `tools/generate-vcu-tables.mts`. Each one's `--check` fails if its output is stale.
- **The decoders match.** About 200,000 synthetic frames go through the reference's decoder. The C++ must produce the same keys, in the same order, with bit-identical values, and warn exactly as often.
  - The frames cover every 11-bit id at every length, payloads shaped to get past each decoder's checks, and GPS sub-frame sequences.
  - It fails if any decoder key never appears.
- **No location reaches the ride log.** The decoded frames go through the signal store, and the check fails if any position signal is written.
- **The pack-temperature routing matches,** through 419 runs of 0x200/0x660 sequences:
  - stock and custom configs, with the setting right and wrong
  - short, long and late frames
  - echo and clamp faults
- **The OBD side matches to the millisecond:** the poller, trouble-code reads, the poller's hold and the E-LOCK read, in 72 scenarios over four simulated hours against a scripted ECU. Every millisecond it must send the same frames, record the same signals, log the same lines and serve the same `/stored-dtcs` body. It also checks all 65,536 codes, every NRC and 94,000 ISO-TP frames. Details are in [`docs/obd.md`](docs/obd.md).
- **Service mode matches to the millisecond.** Its pure parts are checked on random input: codecs, the service gate, rows and diffs. Sweeps and probes, with the OBD poller alongside, run against simulated VCU micros.
- **The write path matches.** The security key, the reply decoders, the allowlist and gates, and the charge commands are checked on random input. Whole write sessions are compared frame for frame on a scripted bus.
- **The charge controller matches** bit for bit, and through scripted charges. Details for service mode, writes and the controller are in [`docs/service-mode.md`](docs/service-mode.md).
- **Numbers and ordering behave as in JavaScript:** 200,000 doubles are written as JavaScript's `String()` writes them, and the event loop keeps Node's order.
- **The settings and consoles behave:**
  - validation and secrets never being printed
  - restart notices and the status report's schedule
  - Wi-Fi staying off without an admin password
  - the Wi-Fi console's login, lockout, telnet handling and line editing
  - HTTP password decoding
- **The signal store and the clock gate behave.** That covers the deadband, non-finite readings and unknown keys. The clock gate must give the reference's verdict on 88,000 randomized offers, including cold boots at 1970, drift, outliers and week-number rollovers.
- **The ride log round-trips.** All the decoded frames go through the whole C++ pipeline onto disk, and are read back exactly. Simulated power cuts and card damage lose only the records they touch. The result imports into `rides.db`. It runs again on a full card, simulated and then as a real FAT image on macOS, and exactly the newest readings must survive.
- **The download is exact.** The ride-log download is checked byte for byte against the writer's real parts:
  - the live part only to its durable length
  - a part deleted mid-download
  - an unmounted card
- **The Bluetooth link works with the dashboard's own code.** The crypto is checked against published vectors and deflate against zlib. The firmware's server, built for the Mac, is driven by the dashboard's `lib/ble-wire.js`. It checks:
  - the handshake, tampering and replay
  - snapshots equal to live state at MTU 185 and 23, and patches
  - every endpoint's body
  - `/dl` while the live stream keeps coming

## Layout

| Path | What |
| --- | --- |
| `components/emcool_can` | Frame decoders and the pack-temperature routing. Pure, and builds on the laptop too. |
| `components/emcool_signals` | Signal registry (generated), live state, the deadband gate, and the ride-log exclusions. |
| `components/emcool_clock` | The GPS clock gate and clock trust. |
| `components/emcool_ride_log` | Ride-log segments, the on-card record format, the file writer, and the download. Plain POSIX calls, so the laptop runs the same code. |
| `components/emcool_ble_wire` | The phone link's wire protocol and session. Crypto is mbedTLS PSA on the device and OpenSSL on the laptop. |
| `components/emcool_ble_server` | The phone link's server: live stream, request handling, the endpoints. Transport-free. |
| `components/emcool_obd` | OBD-II: the PID poller, trouble-code reads over ISO-TP, the poller's hold, the E-LOCK read, the hub's fault list, and Energica's code table (generated). Transport-free. |
| `components/emcool_vcu` | Service mode: the KWP2000 client, sweeps and probes, the service gate, the 29 parameter tables (generated), the snapshots on the card, the write engine and the automatic charge-current controller. Transport-free. |
| `components/emcool_async` | An event loop with Node's ordering rules, and C++20 coroutines on it. |
| `components/emcool_text` | Allocation-free text and JSON building, a window at a time for bodies bigger than any buffer. |
| `components/emcool_settings` | The per-bike settings, the console commands, and the Wi-Fi console's login and line handling. |
| `components/emcool_log` | Logging through ESP_LOG on the device and stderr on the laptop. |
| `main` | Firmware: CAN receive and transmit, the decode, diagnostics, ride-log and Bluetooth tasks, the microSD mount, NimBLE, Wi-Fi and its servers, NVS settings and the serial console, the clock step and the status report. |
| `dashboard` | The phone dashboard: a static web app published to GitHub Pages. |
| `host` | Laptop build of the components, plus the checks. |
| `tools` | Generators, the ride-log reader and import, and `check.sh`. |
| `docs` | Design notes: `ride-log-format.md`, `ble.md`, `obd.md`, `service-mode.md`, `hosting.md`. |

## Decisions

- **The ride log is plain text on a FAT32 microSD card**. Position, heading and altitude are never written to it; the list is in `components/emcool_signals/include/emcool/signals/ride_log_policy.hpp`.
- **The phone dashboard is Bluetooth only.** It is maintained in `dashboard/` and loaded from https://emcool.athegen.com, an origin of its own (the Bluetooth key lives in its `localStorage`). `tools/check-ble.mts` drives the firmware's Bluetooth server with the dashboard's own `lib/ble-wire.js`, so the two are proven to agree.
- **Wi-Fi is for the laptop, and off by default.** It serves the console and the ride log, never the dashboard. It's a station only, never an access point, and it's plain text behind one admin password. See [`docs/wifi.md`](docs/wifi.md).
- **Nothing goes on the bus unless a setting asks for it.** Every transmitting feature defaults to off. With all of them off, the CAN controller is listen-only.

## License

AGPL-3.0. See `LICENSE`.
