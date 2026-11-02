# The phone dashboard over Bluetooth

emcool serves its phone dashboard over Bluetooth LE. The framing and session follow cool-eva's `public/lib/ble-wire.js` (described in cool-eva `docs/ble-server.md`), with one deliberate change: emcool renamed the handshake salts (`emcool/hello`, `emcool/session`) and the request header (`X-Emcool`) so the stack is its own. Open the dashboard in Bluefy on iOS or Chrome on Android. There is no Wi-Fi.

The dashboard itself now lives in this repo at [`../dashboard/`](../dashboard/) — a static site copied from cool-eva's `public/` and rebranded to emcool, paired with this firmware's wire protocol. See its [README](../dashboard/README.md) for how to serve it.

Code:

- `dashboard/`: the phone-side static app (HTML/JS/CSS, VanJS; build-free).
- `components/emcool_ble_wire/`: the protocol and the session.
- `components/emcool_ble_server/`: the server, the live stream and the endpoints.
- `main/ble_link.cpp`: NimBLE.

## Turning it on

Set a link secret of at least 12 characters from the serial console that `idf.py monitor` opens, then restart:

```
emcool> set BLE_SECRET "a phrase you can type on a phone"
emcool> restart
```

With no secret, the bike doesn't advertise, and the boot log says `Bluetooth is off: no link secret`. On the phone, enter the same secret in the dashboard's menu under *Bluetooth* and tap *Connect*. cool-eva's [`docs/connecting-over-bluetooth.md`](https://github.com/andy-siu/cool-eva/blob/main/docs/connecting-over-bluetooth.md) has the phone-side steps.

The bike advertises as **emcool** (`set BLE_NAME …` changes this). The phone finds it by cool-eva's service UUID, not by name, so a Pi running cool-eva would appear in the same list.

The secret lives in NVS, not in the firmware image or the build directory. The console never prints it back. Anyone with the board and a flasher could still read NVS, which is the same exposure as cool-eva's `/etc/default/cool-eva` on the Pi's SD card. ESP32 flash encryption would close that hole, but it can't be undone and complicates reflashing.

## What works

- **The handshake and sealing.** These are cool-eva's: an HMAC proof of the secret, a per-session AES-256-GCM key, a counter per direction with replays dropped, and a 2 s cooldown after a wrong secret. The firmware runs published test vectors through its crypto at boot (RFC 4231 for HMAC-SHA256, GCM test cases 13 and 14), and won't advertise if any fail.
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
  - **Header checks:** a POST needs `X-Cool-Eva: service-mode`, as cool-eva's does.
  - **`/vcu-params`** and **`/vcu-backup`:** read straight off the card, a row at a time.
  - **Freeze frames and the expected-fault list:** `/lifetime-read` and `/freeze-frame-read` (POST, `X-Cool-Eva: service-mode`); `/lifetime-stats` and `/freeze-frames`; `/expected-faults` (GET, and POST with `X-Cool-Eva: expected-faults`). All answered by the diagnostics task.
  - **`/vcu-write`** (GET, POST): the write engine, with `SERVICE_WRITE_ENABLED`. GET returns the allowlist, the gate and the journal and touches nothing, behind no header. POST does one thing to the bike and wants `X-Cool-Eva: service-write` — **deliberately a different value** from the read path's `service-mode`, so a caller that only knows about reads cannot reach a write. See [`service-mode.md`](service-mode.md).
  - **`/charge-auto`** (GET, POST): the automatic charge-current controller, with `CHARGE_AUTO_ENABLED`. GET shows what it is doing (no header); POST `?mode=automatic|off` turns it on or off, behind `X-Cool-Eva: charge-auto`. Answered by the diagnostics task, which owns the controller.
- **Everything else** answers 404, with a body saying the endpoint isn't on emcool yet. That covers `/dl` (ride log download) and fan control. The dashboard shows the error where it would show the result.

## How it differs from cool-eva's server

- **No HTTP tunnel.** cool-eva replays each Bluetooth request into its own HTTP server. emcool has no HTTP server, so requests are answered directly (`DashboardEndpoints`).
- **Response bodies are read a piece at a time.** cool-eva sends a body in 4 KB messages, and so does emcool, but it never holds a whole one: static bodies come out of flash, and `/stored-dtcs` is written out again for each piece from a copy of the lists taken when the request arrived, so the pieces always agree.
- **Snapshots go in chunks.** A snapshot is about 25 KB of JSON, and emcool's messages are capped at about 4.5 KB to fit the ESP32's RAM.
  - This is safe because the dashboard's store (`public/lib/store.js` `apply()`) folds every message in key by key, snapshot or patch alike.
  - One cost: the pack-resistance estimate pairs `pack_v` with `pack_a` only within one message, so a snapshot chunk that splits them gives it no pair. Patches, which carry each 0x200 frame's values together, still do.
- **Real flow control instead of a guessed pace.** cool-eva paces BlueZ notifications at a guessed 24 kB/s.
  - NimBLE has real flow control: when its buffers are full, there's no buffer to be had, and the frame waits.
  - The server builds outbound messages lazily, one of three slots at a time, as notifications drain. So the radio sets the pace, and nothing is queued that has nowhere to go.
- **No shedding.** cool-eva drops patches while its queue is deep. Here a slow link leaves changes marked in the store, and the next patch carries their latest values, so nothing goes stale and nothing is lost.
- **Compression.** ESP-IDF's ROM compressor needs about 165 KB of state, so outbound messages are compressed by a small encoder of its own (`deflate.cpp`): one fixed-Huffman block with LZ77 over a 4 KB window, or a stored block when that's smaller. Inbound messages, which the phone compresses with CompressionStream, go through the ROM's `tinfl`.

## Memory

The server's buffers come from the heap once at boot, about 52 KB:

| Buffer | Size |
| --- | --- |
| Three outbound slots | ~14 KB |
| Session, including the deflate tables | ~24 KB |
| Frame assembler | ~5 KB |
| Header buffer | ~4.5 KB |
| The endpoints' body buffer | 4 KB |

With Bluetooth on, the static-data region is too small for them. The 10 s status line reports free heap and its low point. If the heap runs short on the bike, the first things to shrink are the slot count and the message cap (`kMaxMessageBytes`).

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **`check_ble_wire`.** The crypto matches the published vectors. The deflate encoder round-trips through zlib across sizes and shapes; a deliberately broken distance code fails 1,207 of those checks. The JSON reader rejects malformed and hostile input. Framing works at MTUs from 23 to 512. The handshake works from the phone's side, including a wrong secret, the cooldown, a replay, a recorded HELLO and a sealed envelope with no session.
- **`tools/check-ble.mts`.** The C++ server is driven by cool-eva's own `ble-wire.js`, the code the phone runs, so WebCrypto and CompressionStream sit on the other end. It checks:
  - **Handshake:** the WELCOME opens with the phone's key, and the server nonce rotates afterwards.
  - **Snapshots:** every snapshot equals the server's live state exactly, value and timestamp, with cool-eva's units and groups. This holds at MTU 185 and at MTU 23 (20-byte notifications).
  - **Patches:** they carry the latest values, including after a link that drained nothing for a second.
  - **Requests:** `/status` has the right shape and counts. `/dtc-table` and `/fault-infokeys` are byte-identical to cool-eva's, over seven and three 4 KB pieces. `/stored-dtcs`, after a 60-code mode 03 reply, equals what cool-eva's own `stored-codes.ts` serves for that reply, apart from the read's time. 404 and 400 come back as expected.
  - **Tampering and replay:** a tampered envelope and a replayed one are both dropped, and the session carries on.
  - **Unsubscribe and disconnect:** unsubscribe stops the stream, and a disconnect ends the session.
- **The test can fail.** With the outbound direction byte wrong, the phone can't open anything and the check fails.

**Not verified yet** (nothing has run on the ESP32):

- NimBLE itself: advertising, GATT, MTU exchange and notifications.
- Real throughput, and the heap left over.
- The iOS app or a browser against the real radio.
- The PSA crypto backend and ROM inflate, which are only compiled. The boot self-test covers the crypto the first time it runs.
