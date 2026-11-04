# emcool dashboard

The phone dashboard for emcool: a static web app with no build step (VanJS, no bundler) that talks to the bike over Web Bluetooth.

## Serving it

It's plain static files. Host them anywhere that serves a directory over HTTPS, since Web Bluetooth needs a secure context:

- **GitHub Pages:** this repo has a workflow that publishes `dashboard/` to Pages. Turn it on with **Settings → Pages → Source → GitHub Actions**; the full steps are in [`../docs/github-pages.md`](../docs/github-pages.md). Open the resulting URL in:
  - **Bluefy** on iOS (Safari and Chrome on iOS have no Web Bluetooth)
  - **Chrome** on Android
  - **Chrome** or **Edge** on a Mac (Safari has no Web Bluetooth)
- **Locally:** any static server over HTTPS, or `http://localhost`, which counts as secure. For example, run `python3 -m http.server` and browse to `http://localhost:8000`, though some browsers want real HTTPS for Bluetooth.

There's no server component. The dashboard fetches the bike's endpoints (`/status`, `/dl`, `/vcu-read`, `/vcu-write`, `/charge-auto`, …) by sending HTTP-shaped requests over the Bluetooth link, which the firmware answers. The bike's optional Wi-Fi serves the console and the ride log, never this dashboard.

## Connecting

1. Set a `BLE_SECRET` on the bike (see [`../docs/setup.md`](../docs/setup.md)) and restart it.
2. Open the dashboard, then the menu, and enter the same phrase under **Bluetooth**.
3. Tap **Connect** and pick **emcool** from the browser's list.

The handshake and framing are in `lib/ble-wire.js`. Both ends must agree on these:

- the request header **`X-Emcool`**, whose values (`service-mode`, `service-write`, `charge-auto`, `expected-faults`, `fan`) say what a request that changes something is for
- the handshake's HMAC labels **`emcool/hello`** and **`emcool/session`**

`tools/check-ble.mts` runs the firmware's Bluetooth server, built for the Mac, with this `lib/ble-wire.js` as the phone side. That proves the two still agree.

The phone keeps its preferences in `localStorage` under `emcool.*` keys: the link secret, theme, units, and the light limit. None of these touch the bike.

`vendor/` is VanJS, MIT-licensed, unchanged.
