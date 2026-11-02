# emcool dashboard

The phone dashboard for emcool — a static, build-free web app (VanJS, no bundler) that talks to the bike over Web Bluetooth. Copied from cool-eva's `public/` and rebranded to emcool; the wire protocol was renamed to match emcool's firmware (see below), so this copy pairs with **this** repo's firmware, not cool-eva's.

## Serving it

It is plain static files — host them anywhere that serves a directory over HTTPS (Web Bluetooth needs a secure context):

- **GitHub Pages:** point Pages at this `dashboard/` directory (or copy it to a `docs/`/`gh-pages` root). Open the URL in **Bluefy** on iOS or **Chrome** on Android.
- **Locally:** any static server over HTTPS (or `http://localhost`, which counts as secure). For example `python3 -m http.server` then browse `http://localhost:8000`, though some browsers want real HTTPS for Bluetooth.

There is no server component here and the ESP32 serves nothing over HTTP — the bike link is Bluetooth only. The pages fetch the bike's endpoints (`/status`, `/vcu-read`, `/vcu-write`, `/charge-auto`, …) by tunnelling HTTP-shaped requests over the BLE link, which the firmware answers.

## Connecting

Set a `BLE_SECRET` on the bike (see [`../docs/setup.md`](../docs/setup.md)), enter the same phrase in the dashboard's menu under **Bluetooth**, and tap **Connect**. The handshake and framing are in `lib/ble-wire.js`.

## What was renamed from cool-eva

Branding — page titles, the PWA manifest, the header label, the CSS banner — all now say **emcool**. Two things are wire protocol and were renamed **in lockstep with the firmware**, so the two ends still agree:

- the request header **`X-Emcool`** (was `X-Cool-Eva`) — its values (`service-mode`, `service-write`, `charge-auto`, `expected-faults`, `fan`) are unchanged;
- the handshake HMAC salts **`emcool/hello`** and **`emcool/session`** (were `cool-eva/*`), in `lib/ble-wire.js`.

Client-only names — the `emcool.ble-secret` / `emcool.lightHealthyMax` localStorage keys and the `emcool.celog` download filename — were rebranded too; they touch nothing on the bike.

`host/tests` → `tools/check-ble.mts` drives the firmware's C++ BLE server with **this** `lib/ble-wire.js` as the phone side, so the rename is proven to still handshake. `vendor/` is VanJS, MIT-licensed, unchanged.
