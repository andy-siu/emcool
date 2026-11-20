# Wi-Fi: the consoles and the ride log over the network

Wi-Fi is off until a network and a password are set. Then the bike joins your network as a client and serves:

- **The SSH console** on port 22, behind `ADMIN_PASSWORD`: every command the USB console has, with the log following it. That includes settings, `restart`, firmware updates from the card, the card's commands, and the manual CAN commands (`can-send`, `can-dump`, `psu`, `poller`, `can-repeat`). It's encrypted.
- **The ride log** on port 80, behind `TELNET_PASSWORD`: a page listing every boot and part, and downloads of them.
- **The telnet console** on port 23, behind `TELNET_PASSWORD`, only if `TELNET_CONSOLE` is `1`. It's off by default, and **read-only**: it can look (`show`, `status`, `ifconfig`, `update status`, the card's `dir` and `type`) but changes nothing.

The split is about what crosses the network in the clear. The telnet password and everything after it are plain text, so what they unlock can't change the bike. Everything that can goes through SSH.

Bluetooth carries on beside it, sharing the radio.

Code: `main/wifi_link.cpp` (Wi-Fi, the telnet console's socket, the web server), `main/ssh_console.cpp` (the SSH console, on wolfSSH), `components/emcool_settings/src/remote_console.cpp` (the telnet console's login and what it allows, the SSH login's limits), `components/emcool_settings/src/line_editor.cpp` (the SSH console's line editing), `components/emcool_ride_log/src/download.cpp` (what the page lists and serves, shared with Bluetooth's `/dl`). All but the sockets and the library are checked on the laptop.

## Turning it on

From the USB console (`idf.py monitor`):

```
emcool> set WIFI_SSID "garage wifi"
emcool> set WIFI_PASSWORD "the network's password"
emcool> set ADMIN_PASSWORD "a phrase used nowhere else"
emcool> set TELNET_PASSWORD "another phrase, for the ride log"
emcool> restart
```

- **`WIFI_SSID`:** the network to join, 1–32 characters. A phone hotspot works too.
- **`WIFI_PASSWORD`:** that network's password, 8–63 characters. Leave it unset for an open network.
- **`ADMIN_PASSWORD`:** the SSH console's password, 8–128 characters. Unset, SSH is off.
- **`TELNET_PASSWORD`:** what the ride-log page and the telnet console ask for, 8–128 characters. Unset, both are off. It must differ from `ADMIN_PASSWORD`; the bike refuses a match.
- **`TELNET_CONSOLE`:** `1` serves the read-only telnet console on port 23; `0`, the default, doesn't. See [below](#the-telnet-console).

Wi-Fi comes on with `WIFI_SSID` and either password. With only `ADMIN_PASSWORD`, there's SSH and nothing else; with only `TELNET_PASSWORD`, the ride-log page (and telnet, if asked for).

**Upgrading from a build before SSH:** what was saved as `ADMIN_PASSWORD` is now `TELNET_PASSWORD`, so the ride-log page keeps working with the password you had. SSH stays off until you set a new `ADMIN_PASSWORD`, from the USB console or the dashboard. Don't reuse the old one: it has crossed the network in plain text. The telnet console is off after the upgrade too, unless `TELNET_CONSOLE` had been saved as `1`; `set TELNET_CONSOLE 1` brings it back, read-only.

Or from the dashboard: **Menu → Network settings** (also **Settings → Network connections**). It takes the network name, the Wi-Fi password (or *Open network*), the SSH password and the telnet password, and saves them with the same `set` commands over Bluetooth, so the bike checks them the same way. It won't save a network until a password is saved or typed beside it, since Wi-Fi stays off without one. It saves the passwords first and the network last, so a save that stops partway never leaves a network without a password. **Turn Wi-Fi off** runs `unset WIFI_SSID`. **Read status** shows the `ifconfig` report, and **Restart** applies the change.

The boot log says where to find it:

```
I ssh: listening on port 22; host key SHA256:Qm3vN1…
I wifi: connected as 192.168.1.42: ssh admin@192.168.1.42, ride log http://192.168.1.42/, telnet off
```

The 10 s status line repeats the address, and `ifconfig`, on any console, shows the rest:

```
emcool> ifconfig
wifi:     joined, channel 6, signal -58 dBm
ssid:     garage wifi
address:  192.168.1.42
network:  192.168.1.0/24
netmask:  255.255.255.0
gateway:  192.168.1.1
dns:      192.168.1.1
hostname: emcool
mac:      24:6f:28:aa:bb:cc
ssh:      ssh admin@192.168.1.42
host key: SHA256:Qm3vN1…
telnet:   off (TELNET_CONSOLE 0, or no TELNET_PASSWORD)
ride log: http://192.168.1.42/
```

Before it has joined, it gives the SSID it is trying and how many attempts it has made. With Wi-Fi off, it says so and gives the MAC address. A router can reserve the bike an address by that MAC address. The bike asks the router for the hostname `emcool`, so `emcool.local` or `emcool` may work too, depending on the router.

The bike rejoins by itself if the network drops or the router restarts, retrying from 1 s up to once a minute. `unset WIFI_SSID` then `restart` turns Wi-Fi off.

## The SSH console

```bash
ssh admin@192.168.1.42
```

1. **The first time,** ssh shows the bike's host key fingerprint and asks whether to trust it. Check it against `ifconfig`'s `host key:` line on the USB console or the dashboard's **Read status**, then answer `yes`. The bike makes its key the first time SSH starts and keeps it in NVS, so it stays the same across restarts and firmware updates (only `idf.py erase-flash` makes a new one, and ssh will then warn that the key changed).
2. Type `ADMIN_PASSWORD`. The user name is ignored; `admin` is a convention.
3. Then every command the USB console has: `show`, `set`, `unset`, `status`, `ifconfig`, `restart`, `update`, the card's commands, and the CAN commands. `help` lists them; `exit` or Ctrl-D leaves.
4. The log follows the console as it does on the serial port, status reports included. A log line that arrives while you type is printed above your line, which is then shown again. If the network can't keep up, lines are dropped and a note says how many bytes.

**One command at a time from the laptop:**

```bash
ssh admin@emcool.local show
ssh admin@emcool.local update /emcool.bin
ssh admin@emcool.local restart
```

`can-dump` and `can-send -r` run until a key is pressed. Over SSH, any key stops them, and so does closing the connection.

**Editing:** Backspace, Ctrl-U (clear the line) and Ctrl-C (drop the line) work; the arrow keys do nothing. There's no history.

**Limits:**

- One client at a time. A second waits until the first leaves.
- Three wrong passwords end the connection. After three wrong in a row, every attempt fails for 5 s, whoever makes it.
- A client that types nothing for 30 minutes is disconnected; one that hasn't logged in after a minute is too.
- `restart` waits for what the dashboard's restart waits for: a parked bike and a free bus. It says what's in the way.
- Password logins only: no keys, no port forwarding, no agent forwarding, no `scp` or `sftp`. To put firmware on the bike, copy `emcool.bin` onto the card, then `update /emcool.bin`.

**Algorithms** (wolfSSH): curve25519-sha256 (or ecdh-sha2-nistp256) key exchange, an ssh-ed25519 host key, AES-256-GCM, AES-128-GCM or AES-CTR with HMAC-SHA2. Any OpenSSH from the last ten years agrees on these.

## The telnet console

Off by default. To turn it on, with `TELNET_PASSWORD` set:

```
emcool> set TELNET_CONSOLE 1
emcool> restart
```

Then:

```bash
nc 192.168.1.42 23
```

macOS has no `telnet` any more; `nc` is built in, and a telnet client works too.

1. Type `TELNET_PASSWORD`. A telnet client hides it; `nc` shows it as you type.
2. Then the commands that only look: `show`, `status` (a report now), `ifconfig`, `update status`, and the card's `dir`/`ls`, `cd`, `pwd` and `type`/`cat`. `help` lists them; `exit` leaves.
3. Anything that would change the bike — `set`, `unset`, `status on|off|every`, `restart`, `update FILE`, the card's `rm`, `cp`, `mv`, `mkdir`, `rmdir`, and every CAN command — is refused, with a pointer to the SSH console.
4. The log follows it as on the serial port.

The limits are the SSH console's: one client, three tries and the lockout, 30 minutes idle.

`set TELNET_CONSOLE 0` (or `unset TELNET_CONSOLE`) and a restart close port 23 again. The boot log, `ifconfig` and the status line's `wifi:` all say `telnet off`.

## The ride-log page

Open `http://192.168.1.42/` in any browser. It asks for a user name, which can be anything, and `TELNET_PASSWORD`.

The page lists every boot, newest first, and its parts with their sizes. It has these links:

- **Recent ride:** the newest 16 MB, the same as the dashboard's Bluetooth download.
- **Newest 256 MB**, the most one download carries.
- **Each boot**, to download that boot alone.

A download is one `.log` file, the same format as over Bluetooth. Import it as you would the card:

```bash
node tools/import-ride-log.mts emcool-00012.3-00014.1.log --out rides.db
```

From the command line:

```bash
curl -u x:'the telnet password' -OJ 'http://192.168.1.42/dl?mb=256'
curl -u x:'the telnet password' -OJ 'http://192.168.1.42/dl?boot=12&mb=256'
```

`-OJ` saves under the bike's file name. What goes in a download (whole parts, newest first, each cut where its records end) is described in [`ble.md`](ble.md), under `/dl`.

**If a download fails partway** (the card is pulled, or a read fails), the bike drops the connection before the final chunk. `curl` then reports a transfer error, and a browser marks the download failed. A short file is never passed off as whole.

## Firmware updates

Not over HTTP any more: `PUT /update` is gone, since its password crossed the network in plain text. Over the network, it's the SSH console:

```bash
ssh admin@emcool.local update /emcool.bin     # after copying build/esp32s3/emcool.bin onto the card
ssh admin@emcool.local restart
```

or the dashboard's Firmware panel over Bluetooth. The README has the rest, under *Updating without the cable*.

## What the passwords do and don't protect

- **`ADMIN_PASSWORD` never crosses the network in the clear.** SSH encrypts it and everything after it. It unlocks everything: VCU writes and the automatic charge controller (with their settings), the Bluetooth secret, firmware, the CAN bus. So it guards what the Bluetooth secret guards.
- **`TELNET_PASSWORD` does cross it in plain text, and so does everything after it:** the telnet console's output (settings' names and values, never a secret's characters) and the ride log. Anyone on the same network can see it: on an open network that's anyone in range, and on WPA2 it's anyone who knows the network's password. That's why it unlocks nothing that changes the bike.
- **The two must differ.** The bike refuses to save one equal to the other.
- **Use Wi-Fi on a network you control,** like your garage Wi-Fi or your own phone's hotspot, not a public one. SSH is safe on any network; the ride-log page isn't private on one you don't control.
- **Pick passwords used nowhere else.** Neither is the Bluetooth secret, and shouldn't be.
- **Check the host key the first time.** That's what stops someone on the network pretending to be the bike and catching `ADMIN_PASSWORD`.

## What it costs

- **Flash:** Wi-Fi took the firmware from 1.46 MB to 1.74 MB, even after building for size; with the card commands and updates it's 1.81 MB, and with SSH (wolfSSH and wolfCrypt) 2.13 MB. On the S3's 8 MB flash each app slot is 3.875 MB, so there's room; it was the 4 MB table's 1.875 MB slots that made it tight.
- **The firmware is now built for size (`-Os`), not for debugging (`-Og`).** Both stacks don't fit otherwise: the fast instruction RAM overflowed by 2 KB, and the image outgrew its slot. One cost: a crash's backtrace and variables are a little harder to read, since the compiler inlines and reorders more.
- **Wi-Fi features left out** (`sdkconfig.defaults`): being an access point, enterprise (802.1X) logins, OWE, and a receive-path speed-up that lives in instruction RAM. Joining a home network or a phone hotspot with WPA2 or WPA3 needs none of them.
- **The Wi-Fi stack is linked in either way.** Turning Wi-Fi off doesn't make the firmware smaller; it just never starts the radio.
- **RAM:** on the board, starting Wi-Fi takes about 57 KB of internal RAM (98 KB free after Bluetooth, 41 KB after Wi-Fi), and the boot settles at about 13–19 KB free. That's with Wi-Fi's and lwIP's buffers in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). Without that, the boot settled at about 2 KB free, and the driver logged failed 1.7 KB buffer allocations every time a frame arrived. The 10 s status line's `heap … free, … lowest` counts internal RAM only.
- **PSRAM:** the build turns on the S3-WROOM-1-N8R8's 8 MB of octal PSRAM (`sdkconfig.defaults.esp32s3`), for SSH and the Wi-Fi stack's buffers only: `malloc()` never returns it, and FatFs keeps its buffers internal. (Letting `malloc()` use it put the ride log's write buffer there, which made every card write go a sector at a time through a bounce buffer, and the task watchdog fired.) wolfSSL and wolfSSH are built with `XMALLOC_USER`, so all their allocations go to `main/ssh_console.cpp`'s `XMALLOC`, which asks for PSRAM first: the key exchange's numbers, a session's buffers (4 KB windows and packets, not wolfSSH's 128 KB default). (`wolfSSL_SetAllocators` isn't enough on ESP-IDF: wolfSSL's FreeRTOS settings make `XMALLOC` call `pvPortMalloc`, which only gives internal RAM, and never consult the allocators.) Its task's 12 KB stack is internal RAM. A board without PSRAM still boots (`CONFIG_SPIRAM_IGNORE_NOTFOUND`); SSH then takes internal RAM.
- **wolfSSL's ESP32 crypto hardware port is off.** It doesn't build against ESP-IDF 6.1 (it calls clock-gate functions that have moved), so wolfCrypt runs its software code. A login's key exchange is a curve25519 multiply and an Ed25519 signature, which should take well under a second on the S3 in software (not yet timed on the board); after that, AES-GCM in software is ample for a console.
- **Licences:** wolfSSL is GPL-3.0-or-later, and wolfSSH GPL-2.0-or-later, used here under version 3, which the AGPL-3.0 firmware can include.

## Verified, and not

**Verified on the laptop** (`tools/check.sh`):

- The telnet console's login takes the right password and refuses the wrong one. The checks cover:
  - three wrong attempts closing the connection, and the lockout across connections
  - telnet negotiation, CR LF and CR NUL line ends, backspace
  - quoted arguments, an overlong line
- What the telnet console allows: every looking command, and none that saves, deletes, restarts, installs or transmits.
- The SSH login's limiter (three tries, the lockout, an unset password never matching) and its line editing (echo, Backspace, Ctrl-U, Ctrl-C, Ctrl-D, the arrow keys swallowed, an overlong line, no echo without a terminal).
- HTTP Basic decoding, including every padding length and malformed headers.
- The settings: their ranges, Wi-Fi staying off without a password, the two passwords refusing to match, an old `ADMIN_PASSWORD` becoming `TELNET_PASSWORD` with SSH off, and the passwords never being printed.
- The page's listing: every boot and part at the size a download sends, newest boot first.
- The download it serves is the same code as Bluetooth's `/dl`, with the same checks.

**Not verified yet** (nothing has run on the ESP32):

- Joining a network, reconnecting, and Wi-Fi and Bluetooth sharing the radio.
- The consoles and the page on real sockets, and how fast a download goes.
- The SSH console at all: wolfSSH's handshake with OpenSSH, the host key's fingerprint matching what ssh shows, a session's output and keypresses, the log following it, and how long a login takes. The firmware builds; nothing of SSH has run.
- PSRAM coming up on the board, and the heap left with SSH logged in.
- The heap left with both stacks running.
- Whether `-Os` changes anything in the CAN decoding's timing. The host checks, which compare every decoded value with the reference implementation's, don't run the firmware's compiler.
