// @ts-check
// Settings → emcool → Firmware: new firmware over the Bluetooth link. Pick the build's emcool.bin,
// and it goes to the bike a piece at a time (../lib/firmware-image.js) into the spare app slot, the
// same path the console's `update` takes from the card. It runs at the next restart, on trial: a
// crash, or a restart, in its first 30 s goes back to the firmware running now. docs/ble.md.
import van from "../vendor/van-1.6.1.js";
import { request } from "../lib/transport.js";
import { connection } from "../lib/store.js";
import { arm, armDwellElapsed, armed, refuseKeyRepeat } from "../lib/arming.js";
import { BLE_MODE } from "../lib/ble-link.js";
import { checkImage, describeImage, sendImage } from "../lib/firmware-image.js";
import { SettingsCard } from "./settings-fields.js";

const { button, div, input, label, progress } = van.tags;

/**
 * GET /firmware.
 * @typedef {object} FirmwareStatus
 * @property {string} report
 * @property {"idle" | "writing" | "done" | "failed"} state
 * @property {string} source
 * @property {number} received
 * @property {number} total
 * @property {string} problem
 * @property {number} chunkBytes
 * @property {number} chip
 * @property {string} chipName
 */

// ⚠️ Their own keys, in the `<module>:<control>` spelling, as ../lib/arming.js wants.
const SEND_KEY = "firmware-update:send";
const RESTART_KEY = "firmware-update:restart";

const status = van.state(/** @type {FirmwareStatus | null} */ (null));
const statusError = van.state("");
const image = van.state(/** @type {Uint8Array | null} */ (null));
const imageNote = van.state("");
const imageProblem = van.state(false);
const sending = van.state(false);
const sent = van.state(0);
const result = van.state("");
const resultFailed = van.state(false);
const written = van.state(false);
const restartNote = van.state("");
let cancelRequested = false;
let watching = false;

const linkReady = () => connection.val === "live" && BLE_MODE;

async function refreshStatus() {
  try {
    const response = await request("/firmware");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    status.val = /** @type {FirmwareStatus} */ (await response.json());
    statusError.val = "";
  } catch (error) {
    statusError.val = `Not read: ${error instanceof Error ? error.message : "no answer"}.`;
  }
}

/** @param {Event} event */
async function choose(event) {
  const file = /** @type {HTMLInputElement} */ (event.target).files?.[0];
  image.val = null;
  written.val = false;
  result.val = "";
  if (!file) {
    imageNote.val = "";
    return;
  }
  const bytes = new Uint8Array(await file.arrayBuffer());
  const { info, problem } = checkImage(bytes, status.rawVal?.chip ?? null);
  imageProblem.val = info === null;
  if (info === null) {
    imageNote.val = problem;
    return;
  }
  image.val = bytes;
  imageNote.val = `${file.name}: ${describeImage(info)}. ${bytes.length.toLocaleString()} bytes.`;
}

/** @type {import("../lib/firmware-image.js").FirmwarePost} */
async function post(path, body) {
  const response = await request(path, {
    method: "POST",
    headers: { "X-Emcool": "firmware", "Content-Type": "application/octet-stream" },
    body,
  });
  return { status: response.status, reply: /** @type {any} */ (await response.json()) };
}

/** The send's firing site. */
async function performSend() {
  const bytes = image.rawVal;
  const chunkBytes = status.rawVal?.chunkBytes;
  if (!bytes || !chunkBytes) return;
  sending.val = true;
  cancelRequested = false;
  sent.val = 0;
  resultFailed.val = false;
  result.val = "Starting…";
  const started = performance.now();
  try {
    const reply = await sendImage(
      bytes,
      chunkBytes,
      post,
      done => {
        sent.val = done;
        const seconds = (performance.now() - started) / 1000;
        const rate = done / Math.max(seconds, 0.001);
        const left = Math.round((bytes.length - done) / Math.max(rate, 1));
        result.val = `Sending: ${Math.floor((done * 100) / bytes.length)}%, ${(rate / 1024).toFixed(1)} kB/s, about ${left} s left.`;
      },
      () => cancelRequested
    );
    result.val = reply.message;
    resultFailed.val = !reply.ok;
    written.val = reply.ok;
  } catch (error) {
    result.val = `No confirmed reply (${error instanceof Error ? error.message : "no answer"}). Check what the bike says below before trying again.`;
    resultFailed.val = true;
  } finally {
    sending.val = false;
    await refreshStatus();
  }
}

/** The restart's firing site: the console's own `restart`, with its own checks. */
async function performRestart() {
  restartNote.val = "Restarting…";
  try {
    const response = await request("/console", {
      method: "POST",
      headers: { "X-Emcool": "console", "Content-Type": "application/json" },
      body: JSON.stringify({ line: "restart" }),
    });
    const reply = /** @type {{ ok: boolean, output: string, restarting: boolean }} */ (await response.json());
    restartNote.val = reply.restarting
      ? "Restarting into the new firmware. The dashboard reconnects in a few seconds; leave the bike on for 30 s so the new firmware is kept."
      : reply.output.trim();
    if (reply.restarting) written.val = false;
  } catch (error) {
    restartNote.val = `No confirmed reply (${error instanceof Error ? error.message : "no answer"}).`;
  }
}

/** @param {string} key @param {() => void} fire */
function twoTaps(key, fire) {
  return () => {
    if (armed.val !== key) {
      arm(key);
      return;
    }
    if (!armDwellElapsed()) return;
    armed.val = "";
    fire();
  };
}

export function FirmwareUpdate() {
  if (!watching) {
    watching = true;
    // Read again whenever the link comes up: after a restart, that's what says the trial began.
    van.derive(() => {
      if (connection.val === "live" && BLE_MODE) void refreshStatus();
    });
  }
  return SettingsCard(
    "Firmware",
    "Install new firmware over Bluetooth: choose the build's emcool.bin (build/esp32s3/). The bike checks it, writes it beside the running firmware, and runs it at the next restart, on trial: a crash or a restart in its first 30 s goes back to the firmware running now. Refused while the bike is moving or service mode has the bus. Keep the phone near the bike; it takes a few minutes.",
    () =>
      div(
        { class: "settings-note action-note output" },
        !linkReady()
          ? "Connect over Bluetooth to update the firmware."
          : statusError.val || status.val?.report.trim() || "Reading…"
      ),
    div(
      { class: "settings-row" },
      label(
        { class: "settings-note" },
        "Firmware file ",
        input({
          type: "file",
          accept: ".bin,application/octet-stream",
          disabled: () => sending.val || !linkReady(),
          onchange: choose,
        })
      )
    ),
    () => (imageNote.val ? div({ class: `action-note${imageProblem.val ? " failure" : ""}` }, imageNote.val) : div()),
    div(
      { class: "settings-row" },
      div({ class: "settings-note" }, "Readings carry on while it sends, but recording may stutter."),
      () =>
        sending.val
          ? button(
              {
                class: "settings-button",
                onclick: () => {
                  cancelRequested = true;
                  result.val = "Cancelling…";
                },
              },
              "Cancel"
            )
          : button(
              {
                class: "settings-button writes",
                disabled: () => !image.val || !linkReady() || !status.val,
                onkeydown: refuseKeyRepeat,
                onclick: twoTaps(SEND_KEY, () => void performSend()),
              },
              () => (armed.val === SEND_KEY ? "Confirm send" : "Send to bike")
            )
    ),
    () =>
      sending.val && image.val
        ? progress({ class: "firmware-progress", max: image.val.length, value: sent.val })
        : div(),
    () => (result.val ? div({ class: `action-note${resultFailed.val ? " failure" : ""}` }, result.val) : div()),
    () =>
      written.val
        ? div(
            { class: "settings-row" },
            div({ class: "settings-note" }, "Restart to run it. Readings and recording pause for a few seconds."),
            button(
              {
                class: "settings-button writes",
                disabled: () => !linkReady(),
                onkeydown: refuseKeyRepeat,
                onclick: twoTaps(RESTART_KEY, () => void performRestart()),
              },
              () => (armed.val === RESTART_KEY ? "Confirm restart" : "Restart")
            )
          )
        : div(),
    () => (restartNote.val ? div({ class: "action-note" }, restartNote.val) : div())
  );
}
