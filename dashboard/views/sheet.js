// @ts-check

import van from "../vendor/van-1.6.1.js";
import { OwnerActionRow } from "./owner-action.js";
import { knownKeys, valueOf } from "../lib/store.js";
import { bytes } from "../lib/format.js";
import * as units from "../lib/units.js";
import * as theme from "../lib/theme.js";
import { armed } from "../lib/arming.js";
import { FanControl, refreshFanStatus } from "./fan.js";
import { download, request } from "../lib/transport.js";
import { openDestination, destination } from "../lib/destination.js";

const { button, div, h2, h3 } = van.tags;

// The sheet behind the header button: screen and units, links to the pages behind it,
// the fan and the ride-log download.
//
// Everything here is deliberately not on the riding screens. None of it is worth
// looking at at speed, and all of it is worth having when you stop.

/** @typedef {import("../../src/http/status.ts").StatusPayload} StatusPayload */

export const sheetOpen = van.state(false);

/**
 * Opens the sheet and refreshes everything in it.
 *
 * The single entry point on purpose: each section here fetches its own endpoint
 * when it becomes visible rather than polling in the background, so an opener that
 * set `sheetOpen` directly would show one section's stale numbers next to
 * another's fresh ones. Service mode is handed a way to ask whether the sheet is
 * still open, which is half of what stops it polling forever.
 */
export function openSheet() {
  sheetOpen.val = true;
  // ⚠️ Re-opening the sheet must never find a half-confirmed Update waiting for its second
  // tap. Spelled here rather than left to refreshServiceMode()'s disarm: the sheet's own
  // controls are this file's to reset, not another module's to reset for it.
  // scripts/check-arming.ts asserts this line, and reads this file only because of it.
  armed.val = "";
  void refreshStatus();
  void refreshFanStatus();
}

export const status = van.state(/** @type {StatusPayload | null} */ (null));

export function Sheet() {
  return div(
    {
      class: () => `sheet${sheetOpen.val ? " open" : ""}`,
      // Tapping the backdrop closes it; taps inside must not bubble out to that.
      onclick: () => {
        sheetOpen.val = false;
      },
    },
    div(
      {
        class: "sheet-body",
        onclick: (/** @type {Event} */ event) => event.stopPropagation(),
      },
      div(
        { class: "settings-row" },
        h2({ class: "sheet-heading" }, "Menu"),
        button(
          {
            class: "settings-button",
            onclick: () => {
              sheetOpen.val = false;
            },
          },
          "Close"
        )
      ),
      h2({ class: "sheet-heading" }, "Screen"),
      ThemeToggle(),
      h2({ class: "sheet-heading" }, "Units"),
      UnitsToggle(),
      MenuLink("Settings", "settings"),
      MenuLink("Network info", "network"),
      FanControl(),
      MenuLink("This session", "session"),
      DownloadButton(),
      MenuLink("Bike information", "bike-information"),
      MenuLink("Advanced service", "advanced-service")
    )
  );
}

/** BLE-only: over wifi the browser's download manager shows progress, so this stays empty. */
const downloadProgress = van.state("");

/**
 * How much of the newest ride log one download asks for. The bike sends its newest parts until
 * about this many megabytes (always at least one whole part), each cut where its records end:
 * Bluetooth 4.2 moves tens of kB/s, so this is a recent ride; the card is how to take everything.
 */
const DOWNLOAD_MB = 16;

/** Downloads the newest part of the ride log, for tools/import-ride-log.mts. */
function DownloadButton() {
  return div(
    { class: "settings-card" },
    OwnerActionRow(
      "Ride recording",
      button(
        {
          class: "action",
          onclick: () => {
            if (status.rawVal && !status.rawVal.log.enabled) {
              sheetOpen.val = false;
              openDestination("settings", "recording", openSheet);
              return;
            }
            downloadProgress.val = "";
            downloadCurrentRide((receivedBytes, totalBytes) => {
              downloadProgress.val =
                totalBytes === null ? bytes(receivedBytes) : `${bytes(receivedBytes)} of ${bytes(totalBytes)}`;
            }).then(
              () => {
                downloadProgress.val = "";
              },
              error => {
                console.warn("sheet: ride log download failed", error);
                downloadProgress.val = `failed: ${/** @type {Error} */ (error).message}`;
              }
            );
          },
        },
        () => {
          const current = status.val;
          if (!current) {
            return "⬇  Download recent ride log";
          }
          if (!current.log.enabled) {
            return "⚠  No ride log — no card in the bike";
          }
          // `log.bytes` counts every part at its full size, zero-filled tails and all.
          return `⬇  Download recent ride log (up to ${DOWNLOAD_MB} MB of ${bytes(current.log.bytes)})`;
        }
      ),
      () => (status.val && !status.val.log.enabled ? "Settings" : "Download")
    ),
    () => (downloadProgress.val ? div({ class: "action-note" }, downloadProgress.val) : div())
    // No caption under the button. The log on the card is plain text, not encrypted, and /dl
    // answers only inside the Bluetooth session, so only a phone holding the key can pull it
    // (docs/ble.md, docs/ride-log-format.md).
  );
}

/** True while the bike is reporting at least one stored trouble code. */
export function hasTroubleCodes() {
  return knownKeys.val.some(key => /^dtc_\d+_\d+$/.test(key) && (valueOf(key) ?? 0) > 0);
}

/**
 * Dark/light as a three-button segmented control, the same `.toggle-row` as the units
 * row below it. AUTO follows the bike's own day/night flag while it is broadcasting and
 * the phone's setting otherwise; the two explicit choices are never overruled by either,
 * which is what makes this the escape hatch if the bike's flag ever reads wrong.
 */
export function ThemeToggle() {
  return div(
    { class: "toggle-row" },
    .../** @type {const} */ (["auto", "light", "dark"]).map(preference =>
      button(
        {
          class: () => (theme.themePreference.val === preference ? "on" : ""),
          onclick: () => theme.setThemePreference(preference),
        },
        preference === "auto" ? "Auto · bike" : preference === "light" ? "Light" : "Dark"
      )
    )
  );
}

/**
 * Metric/imperial as a two-button segmented control, reusing the same `.toggle-row`
 * the charge screen's heatmap uses. `.on` tracks unitSystem, so it also reflects what
 * a reload restored from localStorage.
 */
export function UnitsToggle() {
  return div(
    { class: "toggle-row" },
    .../** @type {const} */ (["metric", "imperial"]).map(system =>
      button(
        {
          class: () => (units.unitSystem.val === system ? "on" : ""),
          onclick: () => units.setUnitSystem(system),
        },
        system === "metric" ? "Metric · km, °C" : "Imperial · mi, °F"
      )
    )
  );
}

/** Refreshes /status while the sheet is open, and once at startup for the log size. */
export async function refreshStatus() {
  try {
    const response = await request("/status");
    status.val = /** @type {StatusPayload} */ (await response.json());
  } catch (error) {
    console.warn("status: could not refresh", error);
  }
}

/** @param {string} title @param {import("../lib/destination.js").Destination} name @param {string} [section] */
function MenuLink(title, name, section = "") {
  return button(
    {
      class: "menu-link",
      onclick: () => {
        sheetOpen.val = false;
        openDestination(name, section, openSheet);
      },
    },
    title,
    " →"
  );
}

/** @param {(receivedBytes: number, totalBytes: number | null) => void} onProgress */
async function downloadCurrentRide(onProgress) {
  return download(`/dl?mb=${DOWNLOAD_MB}`, "emcool.log", onProgress);
}
