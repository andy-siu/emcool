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

In a Claude Code session you can reach the same prompt by typing `! idf.py monitor` so its output lands in the conversation.

With Wi-Fi set up, the same console is also on the network: `nc <bike> 23`, behind `ADMIN_PASSWORD`. See [`wifi.md`](wifi.md).

NVS settings **survive reflashing and firmware updates**. Only `idf.py erase-flash` clears them (which also wipes the Bluetooth secret, so the link goes off until you set one again).

## Commands

There are six, no more:

| Command | What it does |
| --- | --- |
| `show` | Lists every setting: its saved value, `(default)` if nothing is saved, and — for a setting changed since boot — `<- takes effect after restart (running: …)`. |
| `set NAME VALUE` | Saves one setting. Quote a value with spaces: `set BLE_SECRET "my long phrase"`. |
| `unset NAME` | Puts one setting back to its default (removes it from NVS). |
| `status [on \| off \| every SECONDS]` | The periodic status report. `status` prints one now; `status off` / `status on` / `status every SECONDS` (5–3600) change it and save at once. |
| `network` | The Wi-Fi network joined and its signal, the address, netmask, gateway and DNS server it handed out, the hostname and MAC address, and where to find the console and the ride-log page. With Wi-Fi off, says so. |
| `restart` | Reboots, so saved settings take effect. |

There is deliberately **no `get`** — `show` prints everything, and it is short. An unknown name tells you so and points at `show`.

**When a change applies.** Settings are read at boot, so almost every change takes effect at the next `restart`; `show` marks any change still waiting for one. The two status-report settings (`STATUS_REPORT`, `STATUS_INTERVAL_S`) are the exception — they apply the instant they're set. `set`/`unset` tell you which case you're in.

**The secret is never printed back.** `show` reports only whether `BLE_SECRET` is set and its length, never its characters.

## Every setting

Types: **flag** is `0`/`1`; **integer** is a whole number in a range; **text**/**secret** are strings with a length range (a secret is never echoed).

| Setting | Type | Default | Range | Applies | What it does |
| --- | --- | --- | --- | --- | --- |
| `BLE_SECRET` | secret | *not set* | 12–128 chars | restart | The phone dashboard's link secret. Unset keeps Bluetooth off entirely. |
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

## A worked example

Enable the live stream and service-mode reads, nothing that writes:

```
emcool> set BLE_SECRET "a phrase you can type on a phone"
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
