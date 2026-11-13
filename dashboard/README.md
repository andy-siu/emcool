# emcool dashboard

The phone dashboard for emcool: a static, build-free web app (VanJS, no bundler) that talks to the bike over Web Bluetooth. It is maintained here, alongside the firmware it pairs with: change it by editing these files directly.

## Serving it

It is plain static files, served over HTTPS (Web Bluetooth needs a secure context):

- **https://emcool.athegen.com**: GitHub Pages with a custom domain, published by this repo's workflow on every push to `main`. [`../docs/hosting.md`](../docs/hosting.md) has the one-time setup. Open it in **Bluefy** on iOS or **Chrome** on Android.
- **Locally:** `python3 -m http.server -d dashboard`, then `http://localhost:8000/?transport=ble`.

Bluetooth opens only from the exact origins in [`lib/hosted-origin.js`](lib/hosted-origin.js), and from `localhost` with `?transport=ble`. Anywhere else, `andy-siu.github.io` included, the page says *"Bluetooth needs its own address"*. The key is kept in the origin's `localStorage`, and a shared origin would let another site read it.

There is no server component here, and the ESP32 serves nothing over HTTP: the bike link is Bluetooth only. The pages fetch the bike's endpoints (`/status`, `/settings`, `/vcu-read`, `/vcu-write`, `/charge-auto`, …) by tunnelling HTTP-shaped requests over the link, which the firmware answers. Endpoints emcool does not serve (`/update`, `/can-restart`, the hub scan) get a 404 or a refusal saying why.

## Connecting

1. **Set the key:** `set BLE_SECRET <your key>` on the bike's serial console; Bluetooth turns on at once ([`../docs/ble.md`](../docs/ble.md#turning-it-on)).
2. **Connect:** open **Settings → Connections**, paste the key under **Enter the bike's Bluetooth key** and tap **Connect this phone**. The browser's device chooser opens; pick **emcool**.

After a reload, the page reconnects by itself to the one device this browser was already allowed to use. If that device is not the bike (a board from earlier testing, say), the link sits on *"Link lost. Reconnecting."* and the bike's console never logs a connection. Tap **Forget this bike & choose another** in the same section: it drops the remembered device, asks the browser to forget it so a reload does not pick it up again, and opens the chooser.

This is protocol 2; the handshake and framing are in `lib/ble-wire.js`, `lib/ble-handshake.js` and `lib/ble-link.js`.

**Settings** (Menu → Settings) shows what the bike runs with, read from `GET /settings`, but saving on those pages is refused. Settings change through the bike's console commands, in **Settings → Bike console** (`views/bike-console.js`, over `/console`) or on the serial console, as [`../docs/setup.md`](../docs/setup.md#the-dashboards-bike-console) describes.

## Names the firmware must agree with

- **Wire protocol, in lockstep with the firmware:**
  - the request header **`X-Emcool`**, with its values `service-mode`, `service-write`, `charge-auto`, `expected-faults`, `fan`, `settings` and `console`;
  - the handshake HMAC salts **`emcool/hello`** and **`emcool/session`**, in `lib/ble-wire.js`.
- **Client-only names:** the `emcool.*` localStorage keys (`emcool.ble-secret`, `emcool.lightHealthyMax`, …) and the download names (`emcool.log`, `emcool-bluetooth-key.txt`). They touch nothing on the bike.

The iOS app's names (`cooleva:`, `coolEvaNative`) are unchanged, because the app checks them.

`tools/check-ble.mts` drives the firmware's C++ BLE server with **this** `lib/ble-wire.js` as the phone side, so the two ends are proven to still handshake. `vendor/` is VanJS, MIT-licensed, unchanged.
