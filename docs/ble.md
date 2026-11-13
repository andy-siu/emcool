# The phone dashboard over Bluetooth

emcool serves its phone dashboard over Bluetooth LE. The framing and session follow cool-eva's `public/lib/ble-wire.js` (described in cool-eva `docs/ble-server.md`), with one deliberate change: emcool renamed the handshake salts (`emcool/hello`, `emcool/session`) and the request header (`X-Emcool`) so the stack is its own. Open the dashboard in Bluefy on iOS or Chrome on Android. The dashboard has no Wi-Fi path; the optional Wi-Fi ([`wifi.md`](wifi.md)) serves only the console and the ride log.

The dashboard itself lives in this repo at [`../dashboard/`](../dashboard/), paired with this firmware's wire protocol. See its [README](../dashboard/README.md) for how to serve it.

This is **protocol 2**, cool-eva's since its #332. A phone dashboard and the firmware must both speak it: a protocol 1 page is refused with `protocol unsupported; this bike speaks 2`.

Code:

- `dashboard/`: the phone-side static app (HTML/JS/CSS, VanJS; build-free).
- `components/emcool_ble_wire/`: the protocol and the session.
- `components/emcool_ble_server/`: the server, the live stream and the endpoints.
- `main/ble_link.cpp`: NimBLE.

## Turning it on

Set the Bluetooth key on the serial console that `idf.py monitor` (or `make monitor`) opens. Bluetooth turns on at once, with no restart:

```
emcool> set BLE_SECRET <your key>
BLE_SECRET saved; Bluetooth is on now. Enter the same key in the dashboard and tap Connect
```

Keep a copy of the key: a password manager, or the note you'll paste it from. On the phone, open the dashboard's *Settings → Connections*, paste the key under *Enter the bike's Bluetooth key*, and tap *Connect this phone*. A second phone gets the same key.

- **The key:** 8-64 printable ASCII characters with at least one letter and one digit, and no spaces, quotes or backslashes, or 64 hex characters. `openssl rand -hex 32` makes a random one, and so does the Bike console's *New key*. A 64-hex key pasted in upper case is lowered, as the dashboard does; a typed key keeps its case, so `Bike2024` and `bike2024` are different keys. Anything else is refused, and the dashboard checks the same rule.
- ⚠️ **A short typed key is weak.** Protocol 2 has no lockout after a wrong key, and anyone who records one connection's HELLO can test guesses against it offline, as fast as their computer allows. An 8-character key can fall in hours to days. Use a random 64-hex key, all the more with `SERVICE_WRITE_ENABLED` on.
- **Once per boot:** the first `set BLE_SECRET` with Bluetooth off starts it. Once Bluetooth is on, a new key is saved for the next `restart`, and phones holding the running key stay connected until then; `show` marks it as waiting. If the start fails (the log says why), the key is still saved and `restart` tries again.
- **Over Wi-Fi:** `set BLE_SECRET` works on the Wi-Fi console too, but the key then crosses the network in plain text. Prefer the USB console.
- **An old phrase:** a key saved before this firmware with a space, or with no letter or no digit, no longer validates. The boot log warns, and Bluetooth stays off until you set a new one.

With no key, the bike doesn't advertise, and the boot log says `Bluetooth is off: no Bluetooth key`. cool-eva's [`docs/connecting-over-bluetooth.md`](https://github.com/andy-siu/cool-eva/blob/main/docs/connecting-over-bluetooth.md) has the phone-side steps.

After a reload or relaunch, the page asks the browser (`navigator.bluetooth.getDevices()`) for devices this origin was already allowed to use. If there is exactly one and a key is stored, it reconnects to that device with no tap. If there are none, more than one, or the browser has no `getDevices`, tap **Connect** as before. ⚠️ cool-eva hasn't confirmed this on a phone yet, in Bluefy or Chrome.

The bike advertises as **emcool** (`set BLE_NAME …` changes this). The phone finds it by cool-eva's service UUID, not by name, so a Pi running cool-eva would appear in the same list.

The key lives in NVS, not in the firmware image or the build directory. The console never prints it; `show` says only `set`. The bike never sends it back:

- **The phone's copy:** after a `wrong secret`, tick **Show key** under the dashboard's Bluetooth box. It shows the key that page holds, in full, so you can compare it with your saved copy. Each origin keeps its own copy.
- **Settings → Dashboard Bluetooth → Show key:** on cool-eva this reads the Pi's saved and active keys back (`POST /settings/bluetooth-key`, reachable over the Pi's Wi-Fi). emcool refuses it with a 405, and the page shows why beside the button. On emcool that request can only arrive over a Bluetooth session, which means the phone already holds the running key. The only new thing it could learn is a replacement key saved but not yet restarted into, and that key is meant to lock old phones out. If every copy is lost, set a new `BLE_SECRET` on the USB console and restart. Anyone with the board and a flasher could still read NVS, which is the same exposure as cool-eva's `/etc/default/cool-eva` on the Pi's SD card. ESP32 flash encryption would close that hole, but it can't be undone and complicates reflashing.

## What works

- **The handshake and sealing.** These are cool-eva's protocol 2 (cool-eva `docs/ble-server.md` §"Key and protocol"):
  - an HMAC proof of the key, and a per-session AES-256-GCM key;
  - a counter per direction, with replays dropped;
  - the server nonce rotated only on success;
  - a refusal that carries the HELLO's client nonce, so the phone can tell it is the answer to its own attempt;
  - no lockout.

  The firmware runs published test vectors through its crypto at boot (RFC 4231 for HMAC-SHA256, GCM test cases 13 and 14), and won't advertise if any fail.
- **Bounds before authentication,** as cool-eva's hardened server:
  - only a whole HELLO in one frame (`FIRST|LAST`, a clear envelope) is considered; anything else from a phone without a session is dropped unanswered;
  - an empty continuation frame resets the assembler;
  - at most four clear refusals go out per 2 s;
  - rejected input is summarised in one log line per 2 s, so a flood can't bury the console.
- **Notification size:** at most min(MTU − 5, 512) bytes, as cool-eva sends. That is 18 at MTU 23 and 180 at MTU 185.
- **The live stream.** `SUBSCRIBE` gets a full snapshot, then patches every 100 ms and a fresh snapshot every 5 s, as on cool-eva.
- **`GET /status`.** It returns cool-eva's `StatusPayload` shape, so the dashboard's sheet works unchanged:
  - **groups:** per-group live counts.
  - **log:** the ride-log parts on the card, at their allocated size.
  - **disk:** the card's free and total space.
  - **waypoints:** always zero, since waypoints were dropped with location logging.
- **The Faults tab's three endpoints.** None of them touches the bus; like cool-eva's, they serve what is already known.
  - **`GET /dtc-table`** and **`GET /fault-infokeys`:** Energica's code table (25 KB) and per-fault shortlists (12 KB), byte for byte cool-eva's. They're generated from cool-eva's own `buildPayload()` by `tools/generate-diagnostic-tables.mts` and served straight from flash, whether or not OBD is on. They're what names the active hub codes (`dtc_CCCC_S`) the bike broadcasts.
  - **`GET /stored-dtcs`:** the OBD trouble-code lists as the poller last read them, in cool-eva's `TroubleCodeSnapshot` shape. With `OBD_ENABLED` off, every list says `not-read`, as cool-eva's does. See [`obd.md`](obd.md).
- **Service mode's endpoints,** in cool-eva's shapes. See [`service-mode.md`](service-mode.md).
  - **`/vcu-read`** (GET, POST, DELETE) and **`/vcu-probe`** (POST): answered by the diagnostics task, which owns the bus. The server waits for the answer without holding up the stream, for up to cool-eva's three minutes.
  - **Header checks:** a POST needs `X-Emcool: service-mode`, as cool-eva's does.
  - **`/vcu-params`** and **`/vcu-backup`:** read straight off the card, a row at a time.
  - **Freeze frames and the expected-fault list:** `/lifetime-read` and `/freeze-frame-read` (POST, `X-Emcool: service-mode`); `/lifetime-stats` and `/freeze-frames`; `/expected-faults` (GET, and POST with `X-Emcool: expected-faults`). All answered by the diagnostics task.
  - **`/vcu-write`** (GET, POST): the write engine, with `SERVICE_WRITE_ENABLED`. GET returns the allowlist, the gate and the journal and touches nothing, behind no header. POST does one thing to the bike and wants `X-Emcool: service-write` — **deliberately a different value** from the read path's `service-mode`, so a caller that only knows about reads cannot reach a write. See [`service-mode.md`](service-mode.md).
  - **`/charge-auto`** (GET, POST): the automatic charge-current controller, with `CHARGE_AUTO_ENABLED`. GET shows what it is doing (no header); POST `?mode=automatic|off` turns it on or off, behind `X-Emcool: charge-auto`. Answered by the diagnostics task, which owns the controller.
- **`GET /dl`: the ride log off the card.** The sheet's *Download recent ride log* button, in Bluefy on iOS or Chrome on Android, and on a Mac in Chrome or Edge (Safari has no Web Bluetooth).
  - **What it sends:** the newest parts, whole, until the next would take it past `?mb=` megabytes (default 16, at most 256). At least one part always goes. They come oldest first, each cut where its records end, so a part's zero-filled tail never goes over the radio. `?boot=12` limits it to `RIDES/00012`.
  - **What you get:** one file, named for the first and last part it holds (`emcool-00012.3-00014.1.log`). Records frame themselves, so the parts laid end to end read like the card's own files. Import it the same way: `node tools/import-ride-log.mts emcool-00012.3-00014.1.log --out rides.db`.
  - **The part being written** goes only as far as the writer has fsynced. The part it's preparing next, all zeros, is left out.
  - **How fast:** the link compresses the log about 3.6× (measured on the corpus), so 16 MB is about 4.5 MB over the air. Bluetooth 4.2's real rate on this board hasn't been measured. At cool-eva's guessed 24 kB/s that's about three minutes. The phone holds the file in memory until it's saved, which is why there's a cap. For the whole card, take the card out.
  - **The live stream keeps going.** While both have something to send, a download and the stream each keep a message slot, so the dashboard never thinks the link has died during a long download.
  - **Planning reads the card** a step at a time: one directory listing, or one part measured by bisecting for its last record (about 14 sector reads for an 8 MB part). So the link task never waits long.
  - **If something goes wrong partway,** the response ends with an error and the dashboard says it failed, never handing over a short file as whole. That covers a card pulled or a file read failing. If the phone goes away, the bike lets go of the file.
  - **A part deleted to make room** during a download (only the oldest, on a full card) goes as zeros, or as what its freed clusters then hold. The laptop's reader steps over both by CRC and drops records it has already seen.
  - **Who can download:** only a phone holding the Bluetooth key, like every other request. The log isn't encrypted on the card or in the file.
- **`GET /settings`:** cool-eva's `SettingsReply`, read-only, built once at boot from the console's settings. The dashboard's riding views read it:
  - Cooling says the coolant probes and fan are off (not ported yet);
  - Charge links to the automatic-current permission (`CHARGE_AUTO_ENABLED`);
  - the header raises a fault if there is one, and there never is.

  The Settings pages open and show these values, under a card pointing at **Bike console**, which is where they change (below). A `PATCH /settings` or anything under `/settings/` answers 405 with cool-eva's `{ ok: false, message }`, so the page says why beside the button. `POST /settings/bluetooth-key` gets a 405 too, in cool-eva's `BluetoothKeyReply` shape, with `saved` and `active` both null (see [Turning it on](#turning-it-on)). `instanceId` is the factory MAC in a UUID's shape, and `processGeneration` is the boot's ride-log session id, so the dashboard can tell a restart from a reconnect.
- **`/console`: Settings → Bike console.** These are the serial console's commands, over the link (`console_endpoints.hpp`; the rider's side is in [`setup.md`](setup.md#the-dashboards-bike-console)). It is emcool's own; cool-eva has nothing like it.
  - **`GET /console`:** every setting as JSON. Each has its name, type, saved value, value in use now, whether it is a default, whether it is waiting for a restart, whether it applies at once, its range and its help. There is also the `revision`, a count of saves since boot from any console. Secrets come back only as `set (N characters)` or `not set`.
  - **`GET /console/status`:** the status report as text, then the `network` report. The decode task owns the report's counters, so it writes the text when it next runs, and the request waits for it, as a service request does.
  - **`POST /console`**, behind `X-Emcool: console`, takes a JSON body of `{"line": "set OBD_ENABLED 1", "revision": 7}`. The line is split into words as the consoles split it. It answers `{ok, output, revision, restarting}`. Its rules:
    - **The revision:** a line that saves needs the `revision` the page read, and gets a 409 if anything has been saved since. The check and the save happen together, under the lock all three consoles take for a command.
    - **Refusals:** a missing header gets the 403 shape that tells the page to reload. An unknown command and anything but one line of at most 512 characters and eight words get a 400.
    - **`restart`:** answered 409 when the bike isn't parked or the bus is in use. If a new `BLE_SECRET` is saved, it also needs `"bluetoothKey"` in the body to match that key. The bike restarts 1.5 s after the reply.
  - **Request bodies:** the server now keeps a REQUEST's body, up to 1 KB, and hands it to the endpoint. A larger body gets the server's own 413, and is never cut short and acted on.
- **Everything else** answers 404, with a body saying the endpoint isn't on emcool yet. That covers fan control, `/update`, `/can-restart`, `/recording/flush` and `/waypoint`. The dashboard shows the error where it would show the result.

## How it differs from cool-eva's server

- **No HTTP tunnel.** cool-eva replays each Bluetooth request into its own HTTP server. emcool has no HTTP server, so requests are answered directly (`DashboardEndpoints`).
- **Response bodies are read a piece at a time.** cool-eva sends a body in 4 KB messages, and so does emcool, but it never holds a whole one: static bodies come out of flash, and `/stored-dtcs` is written out again for each piece from a copy of the lists taken when the request arrived, so the pieces always agree.
- **Snapshots go in chunks.** A snapshot is about 25 KB of JSON, and emcool's messages are capped at 2.5 KB to fit the ESP32's RAM.
  - This is safe because the dashboard's store (`public/lib/store.js` `apply()`) folds every message in key by key, snapshot or patch alike.
  - One cost: the pack-resistance estimate pairs `pack_v` with `pack_a` only within one message, so a snapshot chunk that splits them gives it no pair. Patches, which carry each 0x200 frame's values together, still do.
- **Real flow control instead of a guessed pace.** cool-eva paces BlueZ notifications at a guessed 24 kB/s.
  - NimBLE has real flow control: when its buffers are full, there's no buffer to be had, and the frame waits.
  - The server builds outbound messages lazily, one of three slots at a time, as notifications drain. So the radio sets the pace, and nothing is queued that has nowhere to go.
- **No shedding.** cool-eva drops patches while its queue is deep. Here a slow link leaves changes marked in the store, and the next patch carries their latest values, so nothing goes stale and nothing is lost.
- **Compression.** ESP-IDF's ROM compressor needs about 165 KB of state, so outbound messages are compressed by a small encoder of its own (`deflate.cpp`): one fixed-Huffman block with LZ77 over a 4 KB window, or a stored block when that's smaller. Inbound messages, which the phone compresses with CompressionStream, go through the ROM's `tinfl`.

## Memory

The server's buffers come from the heap once, when Bluetooth starts, about 37 KB. Each is sized by the message cap (`kMaxMessageBytes`, 2.5 KB), which was 4.5 KB until Bluetooth, OBD and the card together ran the heap out at boot:

| Buffer | Size |
| --- | --- |
| Three outbound slots | ~7.6 KB |
| Session, including the deflate tables | ~18 KB |
| Frame assembler | ~2.5 KB |
| Header buffer | ~2.5 KB |
| The endpoints' body buffer | 4 KB |

Response bodies go in 2 KB pieces. The phone joins them, so a smaller piece only means more messages.

A `/dl` in progress adds one open file on the card, which FatFs allocates when it's opened (a `FIL` and its 4 KB sector buffer, since `FATFS_USE_DYN_BUFFERS` is on). The mount allows six open files: the ride log's two, two for service mode's snapshots, the download's one, and one to spare.

With Bluetooth on, the static-data region is too small for them. The boot log prints the heap left after each step, `heap after Bluetooth: …` among them, and the 10 s status line reports free heap and its low point. The ESP-IDF side is trimmed in `sdkconfig.defaults`: the controller allows one connection, not three, and NimBLE keeps about half its default buffers. If the heap still runs short, the next things to shrink are the slot count and the message cap.

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **`check_ble_wire`.** The crypto matches the published vectors. The deflate encoder round-trips through zlib across sizes and shapes; a deliberately broken distance code fails 1,207 of those checks. The JSON reader rejects malformed and hostile input. Framing works at MTUs from 23 to 512. The handshake works from the phone's side, including typed keys accepted (and refused in the wrong case), a phrase or malformed key refused, a wrong key refused to its own nonce and the right one let straight in after it, a replay, a recorded HELLO, a clear HELLO through the assembled path, malformed nonces left unanswered, a protocol 1 HELLO refused by version, and a sealed envelope with no session dropped. An empty continuation resets the assembler.
- **`tools/check-ble.mts`.** The C++ server is driven by cool-eva's own `ble-wire.js`, the code the phone runs, so WebCrypto and CompressionStream sit on the other end. It checks:
  - **Handshake:** a wrong key's refusal carries its nonce; a burst of wrong HELLOs gets four refusals per 2 s and no more; non-HELLO frames before authentication are dropped unanswered; the WELCOME opens with the phone's key, and the server nonce rotates afterwards.
  - **Snapshots:** every snapshot equals the server's live state exactly, value and timestamp, with cool-eva's units and groups. This holds at MTU 185 (180-byte notifications) and at MTU 23 (18-byte).
  - **Patches:** they carry the latest values, including after a link that drained nothing for a second.
  - **Requests:** `/status` has the right shape and counts. `/settings` carries every field the dashboard reads, with this boot's values, and a PATCH or a `/settings/…` action is refused as JSON. `/console` lists the settings without the key, and saves a `set` sent as a request body. It refuses a stale revision, a missing header and an oversized body (413). `/console/status` comes back as text. `/settings/bluetooth-key` is refused without the key anywhere in the body. `/dtc-table` and `/fault-infokeys` are byte-identical to cool-eva's, over seven and three 4 KB pieces. `/stored-dtcs`, after a 60-code mode 03 reply, equals what cool-eva's own `stored-codes.ts` serves for that reply, apart from the read's time. 404 and 400 come back as expected.
  - **`/dl`:** served from the ride log the corpus run wrote, it holds exactly the newest parts' records, byte for byte. The laptop's reader gets the same readings from it as from those parts' files, and it imports through cool-eva's `db.ts`. Also checked:
    - its name, `?boot=`, and the refusals (bad `mb=` or `boot=`, POST, a boot that isn't there)
    - 503 with no card configured
    - at one notification per 10 ms, the stream's messages keep arriving during the body, with no gap near the dashboard's 12 s limit
    - a part truncated mid-download ends the response with an error
    - neither that nor a disconnect mid-download leaves a file open (checked with `lsof`)
    - removing each of those three safeguards (the stream's slot, the error, letting go on disconnect) makes the check fail
  - `check_ride_log` checks the download itself against the writer's real parts:
    - read in pieces of 1 byte to 1 MB
    - budgets, one boot, and the live part only to its durable length, never the next part
    - a part deleted after planning goes as zeros, with every later offset unmoved
    - an unmount mid-download stops it
    - measuring the end of a part's records past a run of zeroed sectors
  - **Tampering and replay:** a tampered envelope and a replayed one are both dropped, and the session carries on.
  - **Unsubscribe and disconnect:** unsubscribe stops the stream, and a disconnect ends the session.
- **The test can fail.** With the outbound direction byte wrong, the phone can't open anything and the check fails.

- **`check_console`.** This drives Bike console's endpoints directly, against an in-memory NVS:
  - **Listing:** every setting is listed, and no secret's characters appear.
  - **Saving:** a `set` saves and moves the revision on. A stale revision, or one made stale by the serial console's save, is refused with nothing saved.
  - **Checks:** the console's own checks still hold, including quoted and escaped values.
  - **Status:** `status` returns the report rather than printing it.
  - **Refusals:** a missing or wrong header, a second line, an overlong line, nine words and an unknown command (`new-ble-key` among them) are all refused.
  - **Restart:** refused while blocked, with the old key or none when a new one is saved, and with the key unset. It goes ahead with the new key, and with the running key saved back.

  Removing the revision check, the secret masking or the restart's key check each makes it fail.

**Not verified yet** (nothing has run on the ESP32):

- NimBLE itself: advertising, GATT, MTU exchange and notifications.
- Real throughput, and the heap left over. That includes how long a 16 MB `/dl` really takes.
- `/dl` reading the card while the ride-log task writes it, on the real FatFs and SPI bus.
- The iOS app or a browser against the real radio.
- The PSA crypto backend and ROM inflate, which are only compiled. The boot self-test covers the crypto the first time it runs.
