# emcool dashboard

The phone dashboard for emcool: a static, build-free web app (VanJS, no bundler) that talks to the bike over Web Bluetooth. It is cool-eva's `public/`, copied and rebranded by `tools/sync-dashboard.mts`, with the wire protocol renamed to match emcool's firmware (see below). So this copy pairs with **this** repo's firmware, not cool-eva's.

## Serving it

It is plain static files, served over HTTPS (Web Bluetooth needs a secure context):

- **https://emcool.athegen.com**: GitHub Pages with a custom domain, published by this repo's workflow. [`../docs/hosting.md`](../docs/hosting.md) has the one-time setup. Open it in **Bluefy** on iOS or **Chrome** on Android.
- **Locally:** `python3 -m http.server -d dashboard`, then `http://localhost:8000/?transport=ble`.

Bluetooth opens only from the exact origins in [`lib/hosted-origin.js`](lib/hosted-origin.js), and from `localhost` with `?transport=ble`. Anywhere else, `andy-siu.github.io` included, the page says *"Bluetooth needs its own address"*. The key is kept in the origin's `localStorage`, and a shared origin would let another site read it.

There is no server component here, and the ESP32 serves nothing over HTTP: the bike link is Bluetooth only. The pages fetch the bike's endpoints (`/status`, `/settings`, `/vcu-read`, `/vcu-write`, `/charge-auto`, …) by tunnelling HTTP-shaped requests over the link, which the firmware answers. Pi-only actions (`/update`, `/can-restart`, the hub scan) get a 404 or a refusal saying why.

## Connecting

1. **Make the key:** run `new-ble-key` on the bike's serial console, then `restart` ([`../docs/ble.md`](../docs/ble.md#turning-it-on)).
2. **Connect:** paste the printed 64-character key in the menu under **Bluetooth → Use an existing access code** and tap **Connect this phone**.

This is protocol 2, cool-eva's since its #332; the handshake and framing are in `lib/ble-wire.js`, `lib/ble-handshake.js` and `lib/ble-link.js`.

**Settings** (Menu → Settings) shows what the bike runs with, read from `GET /settings`, but saving there is refused. Settings change on the serial console (`show`, `set NAME VALUE`, `restart`), as [`../docs/setup.md`](../docs/setup.md) describes.

## Syncing from cool-eva

```bash
node tools/sync-dashboard.mts ../cool-eva          # rewrite dashboard/ from cool-eva's public/
node tools/sync-dashboard.mts ../cool-eva --check  # what tools/check.sh runs: fails if out of date
```

The tool copies `public/` and applies two layers.

- **Renames, everywhere:** the wire names, the brand and the phone-side storage keys (below).
- **Patches, each matching exactly once:**
  - the allowed hosted origin;
  - the ride-log download (emcool's card is plain text, so there is no `/recording/flush`, and Bluetooth is slow, so `?mb=16` of the newest parts);
  - a Settings card saying where settings change.

If a cool-eva change moves a patched line, the tool stops and names it rather than shipping half a patch. `README.md`, `CNAME` and `lib/hosted-origin.js` are emcool's own and survive a sync. Don't edit the other files here by hand: the next sync overwrites them. Change the tool's renames or patches instead.

## What was renamed from cool-eva

- **Branding:** "Cool Eva" becomes **emcool** in page titles, the PWA manifest, the header and the CSS banner.
- **Wire protocol, in lockstep with the firmware**, so the two ends still agree:
  - the request header **`X-Emcool`** (was `X-Cool-Eva`), with its values (`service-mode`, `service-write`, `charge-auto`, `expected-faults`, `fan`, `settings`) unchanged;
  - the handshake HMAC salts **`emcool/hello`** and **`emcool/session`** (were `cool-eva/*`), in `lib/ble-wire.js`.
- **Client-only names:** the `emcool.*` localStorage keys (`emcool.ble-secret`, `emcool.lightHealthyMax`, …) and the download names (`emcool.log`, `emcool-bluetooth-key.txt`). They touch nothing on the bike.

Left as cool-eva's: the wording ("the Pi"), and the iOS app's names (`cooleva:`, `coolEvaNative`), since emcool has no app of its own.

`tools/check-ble.mts` drives the firmware's C++ BLE server with **this** `lib/ble-wire.js` as the phone side, so the renames are proven to still handshake. `vendor/` is VanJS, MIT-licensed, unchanged.
