# Hosting the dashboard

The phone dashboard in [`../dashboard/`](../dashboard/) is a static site. A phone opens it over HTTPS, which Web Bluetooth requires, and from then on talks to the bike directly over Bluetooth LE. Nothing about the ride or the bike's data goes to the host.

It is served by GitHub Pages at **https://emcool.athegen.com**, a custom domain on its own origin.

## Why its own origin

The dashboard keeps the Bluetooth key in the browser's `localStorage`, and `localStorage` belongs to an **origin**, not a path. Any page served from the same origin can read it.

- **The shared Pages origin is out.** `https://andy-siu.github.io/emcool/` shares its origin with every GitHub Pages site on the account. So the dashboard refuses to open Bluetooth there and says *"Bluetooth needs its own address"*.
- **The custom domain is in.** On the custom domain the origin is this dashboard's alone.
- **The allowed list:** [`../dashboard/lib/hosted-origin.js`](../dashboard/lib/hosted-origin.js), exact origins only, with no paths and no wildcards. It is cool-eva's `HOSTED_BLE_ORIGINS` rule (cool-eva `docs/ble-server.md` §"Hosted pages and origin isolation").
- **Moving to another address:** the key doesn't move with it, since storage can't cross origins. Paste it in again from your saved copy.

## One-time setup

1. **Pages source:** in the repo on GitHub, **Settings → Pages → Build and deployment → Source → "GitHub Actions"**. Not "Deploy from a branch": the workflow is the source.
2. **DNS:** at the DNS host for `athegen.com`, add a `CNAME` record `emcool` → `andy-siu.github.io`.
3. **Custom domain:** in **Settings → Pages → Custom domain**, enter `emcool.athegen.com` and save. [`../dashboard/CNAME`](../dashboard/CNAME) carries the same name, so a deploy never resets it.
4. **HTTPS:** once GitHub has checked the DNS and issued the certificate, tick **Enforce HTTPS**.
5. **Optional:** verify the domain for the account (**Settings → Pages → Verified domains** in your profile), so no other repo can claim it.

## How it deploys

[`.github/workflows/pages.yml`](../.github/workflows/pages.yml) uploads the `dashboard/` folder as the Pages artifact and deploys it. It runs:

- automatically on a push to `main` that changed anything under `dashboard/`, and
- on demand: **Actions → "Deploy dashboard to Pages" → Run workflow**, against any branch, to preview one before merging.

There is no build step, since the dashboard is plain HTML/JS/CSS (VanJS). The workflow just packages and ships the folder, and `dashboard/index.html` becomes the site root. `https://emcool.athegen.com/params.html` is the parameters page.

## Opening it on a phone

- **iOS:** open https://emcool.athegen.com in **Bluefy**. Safari and Chrome on iOS have no Web Bluetooth.
- **Android:** open it in **Chrome**.

Then connect to the bike:

1. On the bike's serial console, `set BLE_SECRET <your key>`; Bluetooth turns on at once (see [`ble.md`](ble.md#turning-it-on)).
2. In the dashboard, open **Settings → Connections**, paste the key under **Enter the bike's Bluetooth key** and tap **Connect this phone**.

Load the page while you have reception and keep it open: there is no service worker yet, so in the garage it can't load from scratch. See [`ble.md`](ble.md) for the protocol and [`../dashboard/README.md`](../dashboard/README.md) for what the copy changes from cool-eva.

## Locally

`?transport=ble` on `http://localhost` is also allowed, for development: for example `python3 -m http.server -d dashboard`, then `http://localhost:8000/?transport=ble`.
