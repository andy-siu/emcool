# The phone dashboard over Bluetooth

emcool serves its phone dashboard over Bluetooth LE. The framing and session follow cool-eva's `public/lib/ble-wire.js` (described in cool-eva `docs/ble-server.md`), with one deliberate change: emcool renamed the handshake salts (`emcool/hello`, `emcool/session`) and the request header (`X-Emcool`) so the stack is its own. Open the dashboard in Bluefy on iOS or Chrome on Android. The dashboard has no Wi-Fi path; the optional Wi-Fi ([`wifi.md`](wifi.md)) serves only the console and the ride log.

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
- **`GET /dl`: the ride log off the card.** The sheet's *Download recent ride log* button, in Bluefy on iOS or Chrome on Android, and on a Mac in Chrome or Edge (Safari has no Web Bluetooth).
  - **What it sends:** the newest parts, whole, until the next would take it past `?mb=` megabytes (default 16, at most 256). At least one part always goes. They come oldest first, each cut where its records end, so a part's zero-filled tail never goes over the radio. `?boot=12` limits it to `RIDES/00012`.
  - **What you get:** one file, named for the first and last part it holds (`emcool-00012.3-00014.1.log`). Records frame themselves, so the parts laid end to end read like the card's own files. Import it the same way: `node tools/import-ride-log.mts emcool-00012.3-00014.1.log --out rides.db`.
  - **The part being written** goes only as far as the writer has fsynced. The part it's preparing next, all zeros, is left out.
  - **How fast:** the link compresses the log about 3.6× (measured on the corpus), so 16 MB is about 4.5 MB over the air. Bluetooth 4.2's real rate on this board hasn't been measured. At cool-eva's guessed 24 kB/s that's about three minutes. The phone holds the file in memory until it's saved, which is why there's a cap. For the whole card, take the card out.
  - **The live stream keeps going.** While both have something to send, a download and the stream each keep a message slot, so the dashboard never thinks the link has died during a long download.
  - **Planning reads the card** a step at a time: one directory listing, or one part measured by bisecting for its last record (about 14 sector reads for an 8 MB part). So the link task never waits long.
  - **If something goes wrong partway,** the response ends with an error and the dashboard says it failed, never handing over a short file as whole. That covers a card pulled or a file read failing. If the phone goes away, the bike lets go of the file.
  - **A part deleted to make room** during a download (only the oldest, on a full card) goes as zeros, or as what its freed clusters then hold. The laptop's reader steps over both by CRC and drops records it has already seen.
  - **Who can download:** only a phone holding the link secret, like every other request. The log isn't encrypted on the card or in the file.
- **Everything else** answers 404, with a body saying the endpoint isn't on emcool yet. That covers fan control. The dashboard shows the error where it would show the result.

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

A `/dl` in progress adds one open file on the card, which FatFs allocates when it's opened (a `FIL` and its 4 KB sector buffer, since `FATFS_USE_DYN_BUFFERS` is on). The mount allows six open files: the ride log's two, two for service mode's snapshots, the download's one, and one to spare.

With Bluetooth on, the static-data region is too small for them. The 10 s status line reports free heap and its low point. If the heap runs short on the bike, the first things to shrink are the slot count and the message cap (`kMaxMessageBytes`).

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **`check_ble_wire`.** The crypto matches the published vectors. The deflate encoder round-trips through zlib across sizes and shapes; a deliberately broken distance code fails 1,207 of those checks. The JSON reader rejects malformed and hostile input. Framing works at MTUs from 23 to 512. The handshake works from the phone's side, including a wrong secret, the cooldown, a replay, a recorded HELLO and a sealed envelope with no session.
- **`tools/check-ble.mts`.** The C++ server is driven by cool-eva's own `ble-wire.js`, the code the phone runs, so WebCrypto and CompressionStream sit on the other end. It checks:
  - **Handshake:** the WELCOME opens with the phone's key, and the server nonce rotates afterwards.
  - **Snapshots:** every snapshot equals the server's live state exactly, value and timestamp, with cool-eva's units and groups. This holds at MTU 185 and at MTU 23 (20-byte notifications).
  - **Patches:** they carry the latest values, including after a link that drained nothing for a second.
  - **Requests:** `/status` has the right shape and counts. `/dtc-table` and `/fault-infokeys` are byte-identical to cool-eva's, over seven and three 4 KB pieces. `/stored-dtcs`, after a 60-code mode 03 reply, equals what cool-eva's own `stored-codes.ts` serves for that reply, apart from the read's time. 404 and 400 come back as expected.
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

**Not verified yet** (nothing has run on the ESP32):

- NimBLE itself: advertising, GATT, MTU exchange and notifications.
- Real throughput, and the heap left over. That includes how long a 16 MB `/dl` really takes.
- `/dl` reading the card while the ride-log task writes it, on the real FatFs and SPI bus.
- The iOS app or a browser against the real radio.
- The PSA crypto backend and ROM inflate, which are only compiled. The boot self-test covers the crypto the first time it runs.
