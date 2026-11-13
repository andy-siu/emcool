# Setup: the serial console

Every per-bike option emcool has lives in the ESP32's NVS (non-volatile storage) and is set from the serial console. This is the complete reference: the commands, and every setting with its type, default, range and when it takes effect. The prose rationale for the feature flags is in the README's [Settings](../README.md#settings) section; this is the exact list.

## Opening the console

Flashing opens it:

```
idf.py flash monitor
```

or, on a board that's already flashed, just `idf.py monitor`. It is a UART REPL at the ESP-IDF default (115200 baud, 8N1) over the board's CP2104 USB-C bridge. The prompt is:

```
emcool>
```

With Wi-Fi set up, the same console is also on the network: `nc <bike> 23`, behind `ADMIN_PASSWORD`. See [`wifi.md`](wifi.md).

NVS settings **survive reflashing and firmware updates**. Only `idf.py erase-flash` clears them (which also wipes the Bluetooth key, so the link goes off until you make one again).

The phone dashboard runs the same commands over Bluetooth, in **Menu → Settings → Bike console**. See [The dashboard's Bike console](#the-dashboards-bike-console).

## Commands

There are six, no more:

| Command | What it does |
| --- | --- |
| `show` | Lists every setting: its saved value, `(default)` if nothing is saved, and — for a setting changed since boot — `<- takes effect after restart (running: …)`. |
| `set NAME VALUE` | Saves one setting. Quote a value with spaces: `set WIFI_SSID "garage wifi"`. `set BLE_SECRET <key>` with Bluetooth off turns it on at once; see [`ble.md`](ble.md#turning-it-on). |
| `unset NAME` | Puts one setting back to its default (removes it from NVS). |
| `status [on \| off \| every SECONDS]` | The periodic status report. `status` prints one now; `status off` / `status on` / `status every SECONDS` (5–3600) change it and save at once. |
| `network` | The Wi-Fi network joined and its signal, the address, netmask, gateway and DNS server it handed out, the hostname and MAC address, and where to find the console and the ride-log page. With Wi-Fi off, says so. |
| `restart` | Reboots, so saved settings take effect. |

There is deliberately **no `get`** — `show` prints everything, and it is short. An unknown name tells you so and points at `show`.

**When a change applies.** Settings are read at boot, so almost every change takes effect at the next `restart`; `show` marks any change still waiting for one. Two exceptions apply the instant they're set: the status-report settings (`STATUS_REPORT`, `STATUS_INTERVAL_S`), and `BLE_SECRET` while Bluetooth is off, which turns it on. `set`/`unset` tell you which case you're in.

**Secrets are never printed back.** `show` reports only whether a secret is set (and, for the passwords, its length), never its characters.

## Every setting

Types: **flag** is `0`/`1`; **integer** is a whole number in a range; **text**/**secret** are strings with a length range (a secret is never echoed).

| Setting | Type | Default | Range | Applies | What it does |
| --- | --- | --- | --- | --- | --- |
| `BLE_SECRET` | secret | *not set* | 8–64 chars | **now** if Bluetooth is off, else restart | The phone dashboard's Bluetooth key: 8-64 characters with at least one letter and one digit and no spaces, quotes or backslashes, or 64 hex characters (`openssl rand -hex 32`). A typed key keeps its case. Setting it with Bluetooth off turns Bluetooth on at once; unset keeps it off entirely. See [`ble.md`](ble.md#turning-it-on). |
| `BLE_NAME` | text | `emcool` | 1–29 chars | restart | The name the phone's device list shows. The dashboard finds the bike by its service UUID, not this. |
| `CUSTOM_BMS_CONFIG` | flag | `0` | — | restart | `1` only if the BMS runs the custom LiBAL config, for working out which frame carries the true pack temperature. The frames still win. |
| `OBD_ENABLED` | flag | `0` | — | restart | `1` to poll OBD-II and read trouble codes. This puts the CAN controller on the bus as an active node; off, emcool is listen-only and doesn't even acknowledge frames. |
| `ELOCK_ENABLED` | flag | `0` | — | restart | `1` to read `keys_paired` from the E-LOCK once at startup. Needs `OBD_ENABLED`. |
| `SERVICE_MODE_ENABLED` | flag | `0` | — | restart | `1` to let the dashboard **read** the VCU's calibration parameters, lifetime stats and freeze frames, while the bike is parked and out of drive. Needs `OBD_ENABLED`. See [`service-mode.md`](service-mode.md). |
| `SERVICE_WRITE_ENABLED` | flag | `0` | — | restart | `1` to let the dashboard **write** to the VCU (`/vcu-write`: allowlisted parameters, service point, clock sync, clear DTCs, charge current/stop, SOC limit, reset). The only switch that lets a web page change the bike. Needs `OBD_ENABLED` and `SERVICE_MODE_ENABLED`. See [`service-mode.md`](service-mode.md). |
| `CHARGE_AUTO_ENABLED` | flag | `0` | — | restart | `1` for the automatic charge-current controller — it commands the DC charge current on a 60 s timer to hold the pack near 54 °C. Needs `SERVICE_WRITE_ENABLED` to actuate. See [`service-mode.md`](service-mode.md). |
| `WIFI_SSID` | text | *not set* | 1–32 chars | restart | The Wi-Fi network to join. With `ADMIN_PASSWORD` also set, the bike joins it and serves this console on port 23 and the ride log on port 80. Unset keeps Wi-Fi off. See [`wifi.md`](wifi.md). |
| `WIFI_PASSWORD` | secret | *not set* | 8–63 chars | restart | That network's password. Unset for an open network. |
| `ADMIN_PASSWORD` | secret | *not set* | 12–128 chars | restart | What the Wi-Fi console and the ride-log page ask for. Unset keeps Wi-Fi off. ⚠️ It crosses the network in plain text, and it unlocks everything the console can do. |
| `STATUS_REPORT` | flag | `1` | — | **now** | Whether the serial console prints the periodic status report. `status on` / `status off` is the shorthand. |
| `STATUS_INTERVAL_S` | integer | `10` | 5–3600 | **now** | Seconds between status reports. `status every SECONDS` is the shorthand (and turns the report on if it was off). |

### The transmitting flags, in dependency order

Everything that puts a frame on the bike's bus is off by default. Each layer needs the one before it:

```
OBD_ENABLED=1                 the CAN controller becomes an active node; polls OBD, reads codes
  └ SERVICE_MODE_ENABLED=1    the dashboard may READ the VCU (parameters, lifetime, freeze frames)
      └ SERVICE_WRITE_ENABLED=1    the dashboard may CHANGE the bike (/vcu-write)  ⚠️ off on purpose
          └ CHARGE_AUTO_ENABLED=1  the controller commands the charge current on its own
```

`ELOCK_ENABLED=1` hangs off `OBD_ENABLED` on its own (a one-shot read at startup).

⚠️ `SERVICE_WRITE_ENABLED` is the one that lets a web page change a calibration EEPROM, stamp the service record, clear the fault memory or actuate charging. It is off by default and would be even if the others weren't, and the boot journal says so loudly when it's on.

## When a setting runs the memory out

Each step of startup logs the memory (heap) it leaves, so a boot log shows where it went:

```
I (412) boot: heap after the CAN bus: 187264 free, largest block 110592, lowest so far 187000
I (980) boot: heap after Bluetooth: 61220 free, largest block 31744, lowest so far 60900
```

An allocation that fails logs its size and what was left, as `E (heap) … could not get N bytes`, just before whatever needed it gives up or crashes.

**Safe mode.** If the board crashes (a panic or a watchdog) within 30 s of boot three times in a row, the next boot starts only the serial console. CAN, OBD, the ride log, Bluetooth and Wi-Fi all stay off, so a setting that crashes the boot can be undone:

```
E (40) boot: SAFE MODE: 3 crashes in a row, so only the serial console runs …
emcool> show
emcool> set OBD_ENABLED 0
emcool> restart
```

The `restart` boots normally. A power cycle also leaves safe mode, since it clears the count. If a step of startup fails without crashing, for example on no memory for a buffer, the console still starts, so you can always reach it.

## The dashboard's Bike console

**Menu → Settings → Bike console** runs this console's commands over the Bluetooth link. It writes the same NVS, with the same checks. A change made there is a change made at `emcool>`, and `show` here reports it. It offers:

- **Every setting**, each on its own card: its saved value, the value in use now, a note when it is waiting for a restart, and its help text. Flags get an on/off menu, integers a number field, and text and passwords a text field. **Save** runs `set NAME VALUE`. **Default** runs `unset NAME`. The two status-report settings save as `status on|off` and `status every SECONDS`, so they apply at once, as here.
- **Status**, the status report this console prints, read on request over the window since the last report, followed by the `network` report.
- **Restart**, with two taps.
- **A command line** for `show`, `status`, `network`, `help` and any `set`/`unset`, with a transcript. A secret typed into it shows as `••••••••` in the transcript.

What differs from this console:

- **A new key waits for the restart.** Bluetooth is already on whenever this page can reach the bike, so a new `BLE_SECRET` takes effect at the next restart. The `BLE_SECRET` card's **New key** makes one on the phone and shows it for you to keep before **Save**. `set BLE_SECRET <key>` works too, for a generated key or a typed one.
- **Passwords are never sent back.** As with `show`, a secret reads as `set` or `not set`, plus a password's length.
- **A stale page can't overwrite a newer change.** The page saves against the revision it read. If anything was saved since (here, over Wi-Fi, or from another phone), the save is refused with *the settings changed since this page read them*, and the page reads them again.
- **Two taps for writes.** Turning `SERVICE_WRITE_ENABLED` or `CHARGE_AUTO_ENABLED` on needs a second tap, as the dashboard's other bike-changing controls do. The command line refuses `set` for those two, and refuses `restart`, so the two taps can't be skipped. Turning them off is one tap.
- **Restart is refused while the bike isn't parked.** That means while a fresh reading says it is moving, in drive, being asked for drive, or the throttle is open. It is also refused while a service-mode read or write holds the bus, or while something holds the OBD poller. A charging bike can restart.
- **A restart can't lock the phone out.** If a new `BLE_SECRET` is saved but not yet running, **Restart** asks for that key and the bike checks it before restarting. The phone then keeps it for the reconnect. If `BLE_SECRET` is unset, the page refuses to restart, since Bluetooth would come back off; restart from here or over Wi-Fi.
- **Access:** anyone holding the Bluetooth key can change any setting, including turning on VCU writes, just as anyone with `ADMIN_PASSWORD` can over Wi-Fi. The link is encrypted (AES-256-GCM), unlike the Wi-Fi console. Each change is logged with the setting's name (never its value) under the `console` tag.

The endpoints behind it, `GET /console`, `GET /console/status` and `POST /console`, are in [`ble.md`](ble.md).

## A worked example

Enable the live stream and service-mode reads, nothing that writes:

```
emcool> set BLE_SECRET <your key>
emcool> set OBD_ENABLED 1
emcool> set SERVICE_MODE_ENABLED 1
emcool> show
emcool> restart
```

Later, turn on writing and the automatic charge controller too:

```
emcool> set SERVICE_WRITE_ENABLED 1
emcool> set CHARGE_AUTO_ENABLED 1
emcool> restart
```

Back off a setting:

```
emcool> unset CHARGE_AUTO_ENABLED
emcool> restart
```

Quieten the status report without a restart:

```
emcool> status every 30
emcool> status off
```
