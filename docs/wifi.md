# Wi-Fi: the console and the ride log over the network

Wi-Fi is off until two settings are set. Then the bike joins your network as a client and serves two things, both behind `ADMIN_PASSWORD`:

- **The console** on port 23: the same commands as the USB console, with the log following it.
- **The ride log** on port 80: a page listing every boot and part, and downloads of them.
- **Firmware updates** on port 80: `PUT /update` with the new `emcool.bin`.

Bluetooth carries on beside it, sharing the radio.

Code: `main/wifi_link.cpp` (Wi-Fi, sockets, the web server), `components/emcool_settings/src/remote_console.cpp` (the console's login and line handling, checked on the laptop), `components/emcool_ride_log/src/download.cpp` (what the page lists and serves, shared with Bluetooth's `/dl`).

## Turning it on

From the USB console (`idf.py monitor`):

```
emcool> set WIFI_SSID "garage wifi"
emcool> set WIFI_PASSWORD "the network's password"
emcool> set ADMIN_PASSWORD "a phrase used nowhere else"
emcool> restart
```

- **`WIFI_SSID`:** the network to join, 1–32 characters. A phone hotspot works too.
- **`WIFI_PASSWORD`:** that network's password, 8–63 characters. Leave it unset for an open network.
- **`ADMIN_PASSWORD`:** what the console and the page ask for, 12–128 characters. Without it, Wi-Fi stays off.

The boot log says where to find it:

```
I wifi: connected as 192.168.1.42: console `nc 192.168.1.42 23`, ride log http://192.168.1.42/
```

The 10 s status line repeats the address, and `network`, on either console, shows the rest:

```
emcool> network
wifi:     joined "garage wifi", channel 6, signal -58 dBm
address:  192.168.1.42
netmask:  255.255.255.0
gateway:  192.168.1.1
dns:      192.168.1.1
hostname: emcool
mac:      24:6f:28:aa:bb:cc
console:  nc 192.168.1.42 23
ride log: http://192.168.1.42/
```

Before it has joined, it says what it is trying and how many attempts it has made. A router can reserve the bike an address by that MAC address. The bike asks the router for the hostname `emcool`, so `emcool.local` or `emcool` may work too, depending on the router.

The bike rejoins by itself if the network drops or the router restarts, retrying from 1 s up to once a minute. `unset WIFI_SSID` then `restart` turns Wi-Fi off.

## The console

```bash
nc 192.168.1.42 23
```

macOS has no `telnet` any more; `nc` is built in, and a telnet client works too.

1. Type the admin password. A telnet client hides it; `nc` shows it as you type.
2. Then use the same commands as the USB console: `show`, `set`, `unset`, `status`, `network`, `restart`, plus `help` and `exit`.
3. The log follows the console as it does on the serial port, status reports included. If the network can't keep up, lines are dropped and a note says how many bytes.

**Limits:**

- One client at a time. A second waits until the first leaves.
- Three wrong passwords end the connection. After three wrong in a row, every attempt fails for 5 s, whoever makes it.
- A client that types nothing for 30 minutes (1 minute before logging in) is disconnected.

## The ride-log page

Open `http://192.168.1.42/` in any browser. It asks for a user name, which can be anything, and the admin password.

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
curl -u x:'the admin password' -OJ 'http://192.168.1.42/dl?mb=256'
curl -u x:'the admin password' -OJ 'http://192.168.1.42/dl?boot=12&mb=256'
```

`-OJ` saves under the bike's file name. What goes in a download (whole parts, newest first, each cut where its records end) is described in [`ble.md`](ble.md), under `/dl`.

**If a download fails partway** (the card is pulled, or a read fails), the bike drops the connection before the final chunk. `curl` then reports a transfer error, and a browser marks the download failed. A short file is never passed off as whole.

## Firmware updates

```bash
curl -u x:'the admin password' -T build/emcool.bin http://192.168.1.42/update
```

The bike checks the image as it arrives, writes it into the spare app slot, checks its hash, and answers with what it will run. It runs at the next `restart`, on trial: a crash in its first 30 s goes back to the old firmware. One update at a time, from here or a console's `update`. The README has the rest, under *Updating without the cable*.

⚠️ The admin password is the only thing between this network and new firmware, and it crosses the network in plain text. Anyone who has seen it can install firmware of their own. Use a network you control, or update from the card instead.

## What the password does and doesn't protect

- **The password crosses the network in plain text, and so does everything after it.** That includes the console's commands, `set ADMIN_PASSWORD` itself, and the ride log. Anyone on the same network can see all of it: on an open network that's anyone in range, and on WPA2 it's anyone who knows the network's password.
- **The console can change everything:** turn on VCU writes and the automatic charge controller, change the Bluetooth secret, restart the bike. So the admin password guards what the Bluetooth secret guards, but over a channel that doesn't encrypt it.
- **Use it on a network you control,** like your garage Wi-Fi or your own phone's hotspot, not a public one.
- **Pick an admin password used nowhere else.** It isn't the Bluetooth secret, and shouldn't be.

## What it costs

- **Flash:** Wi-Fi took the firmware from 1.46 MB to 1.74 MB, even after building for size; with the card commands and updates it's 1.81 MB of its 1.875 MB slot. Both slots are in use now (updates write the spare one), so they can't be merged to make room. More room would come from a bigger flash chip, or from shrinking the 128 KB core-dump partition.
- **The firmware is now built for size (`-Os`), not for debugging (`-Og`).** Both stacks don't fit otherwise: the fast instruction RAM overflowed by 2 KB, and the image outgrew its slot. One cost: a crash's backtrace and variables are a little harder to read, since the compiler inlines and reorders more.
- **Wi-Fi features left out** (`sdkconfig.defaults`): being an access point, enterprise (802.1X) logins, OWE, and a receive-path speed-up that lives in instruction RAM. Joining a home network or a phone hotspot with WPA2 or WPA3 needs none of them.
- **The Wi-Fi stack is linked in either way.** Turning Wi-Fi off doesn't make the firmware smaller; it just never starts the radio.
- **RAM:** with Wi-Fi on, the stack takes tens of KB of heap for its buffers, on top of Bluetooth's. That hasn't been measured on the board. The 10 s status line's `heap … free, … lowest` will show it.

## Verified, and not

**Verified on the laptop** (`tools/check.sh`):

- The console's login takes the right password and refuses the wrong one. The checks cover:
  - three wrong attempts closing the connection, and the lockout across connections
  - telnet negotiation, CR LF and CR NUL line ends, backspace
  - quoted arguments, an overlong line
  - commands actually changing settings
- HTTP Basic decoding, including every padding length and malformed headers.
- The new settings: their ranges, Wi-Fi staying off without an admin password, and the passwords never being printed.
- The page's listing: every boot and part at the size a download sends, newest boot first.
- The download it serves is the same code as Bluetooth's `/dl`, with the same checks.

**Not verified yet** (nothing has run on the ESP32):

- Joining a network, reconnecting, and Wi-Fi and Bluetooth sharing the radio.
- The console and the page on real sockets, and how fast a download goes.
- The heap left with both stacks running.
- Whether `-Os` changes anything in the CAN decoding's timing. The host checks, which compare every decoded value with the reference implementation's, don't run the firmware's compiler.
