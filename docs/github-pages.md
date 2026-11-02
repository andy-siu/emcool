# Hosting the dashboard on GitHub Pages

The phone dashboard in [`../dashboard/`](../dashboard/) is a static site. This repo ships a workflow that publishes it to GitHub Pages so you can open it on a phone over HTTPS — which Web Bluetooth requires. The bike link is still Bluetooth; Pages only serves the page.

## One-time setup

1. Push this branch's `.github/workflows/pages.yml` to the default branch (`main`) — merge the PR, or cherry-pick the workflow.
2. In the repo on GitHub: **Settings → Pages → Build and deployment → Source → "GitHub Actions"**. (Not "Deploy from a branch" — the workflow is the source.)

That's it. Pages is now driven by the workflow, not by a branch folder.

## How it deploys

[`.github/workflows/pages.yml`](../.github/workflows/pages.yml) uploads the `dashboard/` folder as the Pages artifact and deploys it. It runs:

- automatically on a push to `main` that changed anything under `dashboard/`, and
- on demand — **Actions → "Deploy dashboard to Pages" → Run workflow** — against any branch, so you can preview a feature branch before merging.

There is no build step; the dashboard is plain HTML/JS/CSS (VanJS), so the workflow just packages and ships the folder. `dashboard/index.html` is the entry point and becomes the site root.

## The URL

A project site is served at:

```
https://andy-siu.github.io/emcool/
```

`dashboard/index.html` is the root, so that URL opens the dashboard; `https://andy-siu.github.io/emcool/params.html` is the parameters page. The exact URL is also printed on each successful deploy (Actions run → the `deploy` job → the environment URL) and under **Settings → Pages**.

> The dashboard uses only relative paths, so it works under the `/emcool/` subpath a project site adds — nothing to configure for that.

## Opening it on a phone

- **iOS:** open the Pages URL in **Bluefy** (Safari and Chrome on iOS have no Web Bluetooth).
- **Android:** open it in **Chrome**.

Then connect to the bike:

1. Set a `BLE_SECRET` on the bike from the serial console (see [`setup.md`](setup.md)) and `restart`.
2. In the dashboard's menu under **Bluetooth**, enter the same phrase and tap **Connect**.

The page talks to the bike directly over Bluetooth LE — GitHub Pages only hosts the files, and nothing about the ride (or the bike's data) goes to GitHub. See [`ble.md`](ble.md) for the protocol and [`../dashboard/README.md`](../dashboard/README.md) for what the copy changed from cool-eva.

## Why not "deploy from a branch"?

GitHub Pages' branch mode can serve a repo's root or its `/docs` folder, but not an arbitrary subfolder like `dashboard/`. The Actions workflow serves `dashboard/` exactly, with no file moves and no duplicate copy to keep in sync. If you ever prefer branch mode instead, publish the *contents* of `dashboard/` to the root of a dedicated `gh-pages` branch and point Settings → Pages at that branch; the workflow here is the simpler path and is what the repo assumes.
