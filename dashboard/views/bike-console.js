// @ts-check
// Settings → Bike console: emcool's serial console over the Bluetooth link. Every setting with
// its saved value and the value in force, a form per setting, the status report the serial port
// prints, and a command line for anything else. It runs the console's own commands (show, set,
// unset, status, network, restart) through /console, so a change here is a change at `emcool>`.
// See docs/setup.md.
import van from "../vendor/van-1.6.1.js";
import { request } from "../lib/transport.js";
import { connection } from "../lib/store.js";
import { arm, armDwellElapsed, armed, refuseKeyRepeat } from "../lib/arming.js";
import { BLE_MODE, rememberSecret } from "../lib/ble-link.js";
import { canonicalBleSecret, generateBleSecret, validBleSecret } from "../lib/ble-secret.js";
import { SettingsCard } from "./settings-fields.js";

const { button, div, input, label, option, pre, select, span } = van.tags;

/**
 * @typedef {object} ConsoleSetting
 * @property {string} name
 * @property {"secret" | "text" | "flag" | "integer"} type
 * @property {string} saved what is saved; a secret only as "set (N characters)" or "not set"
 * @property {string} active what this boot runs with, shown the same way
 * @property {boolean} isDefault nothing is saved for it
 * @property {boolean} pending saved, but waiting for a restart
 * @property {boolean} live applies the moment it is saved
 * @property {boolean} set a secret or text has a value
 * @property {string} defaultValue
 * @property {number} minimum
 * @property {number} maximum
 * @property {string} help
 */
/** @typedef {{ revision: number, settings: ConsoleSetting[] }} ConsoleList */
/** @typedef {{ ok: boolean, output: string, revision: number, restarting: boolean }} ConsoleRun */
/** @typedef {import("../vendor/van-1.6.1.js").State<string>} TextState */

const list = van.state(/** @type {ConsoleList | null} */ (null));
const listError = van.state("");
const busy = van.state("");
const results = van.state(/** @type {Record<string, string>} */ ({}));
const statusReport = van.state("");
const statusError = van.state("");
const transcript = van.state(/** @type {{ line: string, output: string }[]} */ ([]));
const commandLine = van.state("");
// The key a pending BLE_SECRET restart needs: typed, or kept from saving it here.
const restartKey = van.state("");
// A key made here (New key), shown until it is saved.
const generatedKey = van.state("");
const generatedKept = van.state(false);
/** What each card's field holds, so a refresh after another card's save keeps it. @type {Map<string, { value: TextState, edited: boolean }>} */
const drafts = new Map();
let watching = false;

const RESTART_ARM = "bike-console:restart";
const TRANSMITTING = new Set([
  "OBD_ENABLED",
  "ELOCK_ENABLED",
  "SERVICE_MODE_ENABLED",
  "SERVICE_WRITE_ENABLED",
  "CHARGE_AUTO_ENABLED",
]);
// Turning one of these on lets a page holding the Bluetooth key change the bike; it asks twice.
const WRITES = new Set(["SERVICE_WRITE_ENABLED", "CHARGE_AUTO_ENABLED"]);
const OUTPUT_STYLE =
  "font-family: ui-monospace, Menlo, monospace; font-size: 12px; line-height: 1.45; white-space: pre-wrap; overflow-wrap: anywhere; margin: 10px 0 0; color: var(--fg);";

const linkReady = () => connection.val === "live" && BLE_MODE;

async function refreshList() {
  try {
    const response = await request("/console", { cache: "no-store" });
    if (!response.ok) {
      listError.val =
        response.status === 404
          ? "This firmware has no Bike console. Update emcool, or use the serial console."
          : `The bike answered ${response.status}.`;
      return;
    }
    const current = /** @type {ConsoleList} */ (await response.json());
    for (const setting of current.settings) {
      const draft = drafts.get(setting.name);
      if (draft && !draft.edited) draft.value.val = setting.type === "secret" ? "" : setting.saved;
    }
    list.val = current;
    listError.val = "";
  } catch (error) {
    listError.val = `Not read: ${error instanceof Error ? error.message : "no answer"}.`;
  }
}

async function refreshStatus() {
  busy.val = "status";
  try {
    const response = await request("/console/status", { cache: "no-store" });
    const text = await response.text();
    if (!response.ok) {
      statusError.val = `The bike answered ${response.status}: ${text}`;
      return;
    }
    statusReport.val = text;
    statusError.val = "";
  } catch (error) {
    statusError.val = `Not read: ${error instanceof Error ? error.message : "no answer"}.`;
  } finally {
    busy.val = "";
  }
}

/** The line as the transcript shows it: a secret's value hidden. @param {string} line */
function maskedLine(line) {
  const secrets = (list.rawVal?.settings ?? []).filter(setting => setting.type === "secret").map(setting => setting.name);
  const match = /^(\s*set\s+)(\S+)(\s+)(.*)$/i.exec(line);
  if (!match || !secrets.includes(match[2].toUpperCase())) return line;
  return `${match[1]}${match[2]}${match[3]}••••••••`;
}

/**
 * Runs one console line. `key` names where its result shows: a setting's name, "restart" or "command".
 * @param {string} key @param {string} line @param {Record<string, unknown>} [extra]
 * @returns {Promise<ConsoleRun | null>}
 */
async function runLine(key, line, extra = {}) {
  if (!list.rawVal) {
    results.val = { ...results.rawVal, [key]: "Read the settings first." };
    return null;
  }
  busy.val = key;
  results.val = { ...results.rawVal, [key]: "Working…" };
  try {
    const response = await request("/console", {
      method: "POST",
      headers: { "X-Emcool": "console", "Content-Type": "application/json" },
      body: JSON.stringify({ line, revision: list.rawVal.revision, ...extra }),
    });
    const reply = /** @type {ConsoleRun} */ (await response.json());
    results.val = { ...results.rawVal, [key]: reply.output.trim() };
    if (key === "command") {
      transcript.val = [...transcript.rawVal, { line: maskedLine(line), output: reply.output }].slice(-12);
    }
    if (!reply.restarting) await refreshList();
    return reply;
  } catch (error) {
    results.val = {
      ...results.rawVal,
      [key]: `No confirmed reply (${error instanceof Error ? error.message : "no answer"}). Read the settings again before retrying.`,
    };
    await refreshList();
    return null;
  } finally {
    busy.val = "";
  }
}

/** Quotes a value as the console splits words: double quotes group, a backslash escapes. @param {string} value */
function quoted(value) {
  return `"${value.replace(/[\\"]/g, character => `\\${character}`)}"`;
}

export function BikeConsole() {
  if (!watching) {
    watching = true;
    van.derive(() => {
      if (connection.val === "live") void refreshList();
    });
  } else {
    void refreshList();
  }
  return div(
    SettingsCard(
      "The bike's console, over Bluetooth",
      "Runs the serial console's commands: show, set NAME VALUE, unset NAME, status, network and restart. A change here is saved on the bike exactly as at the emcool> prompt. Most settings take effect after Restart; the status report's apply at once. Passwords are never sent back, only whether they are set.",
      () =>
        div(
          { class: "settings-note", role: "status" },
          linkReady() ? listError.val : "Connect over Bluetooth to use the bike's console."
        )
    ),
    StatusCard(),
    () => {
      const current = list.val;
      if (!current) return div();
      return div(RestartCard(current), ...current.settings.map(setting => SettingCard(setting)));
    },
    CommandCard()
  );
}

function StatusCard() {
  return SettingsCard(
    "Status",
    "The status report the serial console prints, read now over the window since the last report, and the network report.",
    div(
      { class: "settings-actions" },
      button(
        {
          class: "settings-button",
          disabled: () => !!busy.val || !linkReady(),
          onclick: () => {
            void refreshStatus();
          },
        },
        () => (busy.val === "status" ? "Reading…" : statusReport.val ? "Read again" : "Read status")
      )
    ),
    () => div({ class: "settings-note", role: "status" }, statusError.val),
    () => (statusReport.val ? pre({ style: OUTPUT_STYLE }, statusReport.val) : div())
  );
}

/** @param {ConsoleList} current */
function RestartCard(current) {
  const pending = current.settings.filter(setting => setting.pending);
  const savedKey = current.settings.find(setting => setting.name === "BLE_SECRET");
  const keyChanging = savedKey?.pending === true && savedKey.saved !== "not set";
  const keyRemoved = savedKey?.pending === true && savedKey.saved === "not set";
  return SettingsCard(
    "Restart emcool",
    pending.length
      ? `Saved, waiting for a restart: ${pending.map(setting => setting.name).join(", ")}.`
      : "No saved changes waiting. A restart pauses readings and recording for a few seconds.",
    keyRemoved
      ? div(
          { class: "settings-note" },
          "BLE_SECRET is unset, so after a restart Bluetooth is off and this page could not reconnect. Restart from the USB or Wi-Fi console."
        )
      : div(),
    keyChanging
      ? div(
          { class: "settings-form-fields" },
          div(
            { class: "settings-note" },
            "A new Bluetooth key is saved, and every phone needs it after the restart. Paste it here: the bike checks it before restarting, and this phone keeps it to reconnect."
          ),
          input({
            "aria-label": "The new Bluetooth key",
            type: "password",
            autocomplete: "off",
            spellcheck: false,
            value: () => restartKey.val,
            oninput: event => {
              restartKey.val = canonicalBleSecret(/** @type {HTMLInputElement} */ (event.target).value);
            },
          })
        )
      : div(),
    div(
      { class: "settings-row" },
      div(
        "Restart",
        div(
          { class: "settings-note" },
          "Refused while the bike is moving or in drive, or while a service read or write is using the bus."
        )
      ),
      button(
        {
          class: "settings-button",
          onkeydown: refuseKeyRepeat,
          disabled: () => !!busy.val || !linkReady() || keyRemoved || (keyChanging && !validBleSecret(restartKey.val)),
          onclick: () => {
            if (armed.val !== RESTART_ARM) {
              arm(RESTART_ARM);
              return;
            }
            if (!armDwellElapsed()) return;
            armed.val = "";
            void restart(keyChanging);
          },
        },
        () => (armed.val === RESTART_ARM ? "Confirm restart" : "Restart")
      )
    ),
    () => div({ class: "settings-note", role: "status" }, results.val.restart ?? "")
  );
}

/** @param {boolean} keyChanging */
async function restart(keyChanging) {
  const key = restartKey.rawVal;
  const reply = await runLine("restart", "restart", keyChanging ? { bluetoothKey: key } : {});
  if (reply?.restarting && keyChanging) {
    // The bike checked it against the saved key; the reconnect after the restart needs it.
    rememberSecret(key);
    restartKey.val = "";
  }
}

/** @param {ConsoleSetting} setting */
function draftFor(setting) {
  let draft = drafts.get(setting.name);
  if (!draft) {
    draft = { value: van.state(setting.type === "secret" ? "" : setting.saved), edited: false };
    drafts.set(setting.name, draft);
  }
  return draft;
}

/** @param {ConsoleSetting} setting */
function SettingCard(setting) {
  const key = setting.name;
  const draft = draftFor(setting);
  const isKey = key === "BLE_SECRET";
  const describe = /** @param {string} value */ value =>
    setting.type === "flag" ? (value === "1" ? "1 (on)" : "0 (off)") : value || "(empty)";
  const turningOnWrites = () => WRITES.has(key) && draft.value.val === "1" && setting.saved !== "1";
  const armKey = `bike-console:${key}`;
  return SettingsCard(
    key,
    setting.help,
    div(
      { class: "settings-note settings-state" },
      div(`Saved: ${describe(setting.saved)}${setting.isDefault ? " (default)" : ""}.`),
      setting.pending
        ? div(`Running now: ${describe(setting.active)}. Takes effect after Restart.`)
        : div(setting.live ? "In effect now." : "In use now.")
    ),
    TRANSMITTING.has(key)
      ? div(
          { class: "settings-note" },
          WRITES.has(key)
            ? "⚠️ Transmits on the bike's bus. On, a page holding the Bluetooth key can change the bike. Saving it on asks twice."
            : "Transmits on the bike's bus when on."
        )
      : div(),
    isKey ? KeyMaker(draft) : div(),
    div(
      { class: "settings-row" },
      div(Editor(setting, draft)),
      button(
        {
          class: () => (turningOnWrites() ? "settings-button writes" : "settings-button"),
          onkeydown: refuseKeyRepeat,
          disabled: () =>
            !!busy.val ||
            !linkReady() ||
            (setting.type === "secret" && !draft.value.val) ||
            (isKey && !!generatedKey.val && draft.value.val === generatedKey.val && !generatedKept.val),
          onclick: () => {
            if (turningOnWrites()) {
              if (armed.val !== armKey) {
                arm(armKey);
                return;
              }
              if (!armDwellElapsed()) return;
              armed.val = "";
            }
            void save(setting, draft);
          },
        },
        () => (turningOnWrites() && armed.val === armKey ? "Confirm save" : "Save")
      ),
      setting.isDefault
        ? div({ hidden: true })
        : button(
            {
              class: "settings-button",
              disabled: () => !!busy.val || !linkReady(),
              onclick: () => {
                draft.edited = false;
                void runLine(key, `unset ${key}`);
              },
            },
            "Default"
          )
    ),
    () => div({ class: "settings-note", role: "status" }, results.val[key] ?? "")
  );
}

/**
 * New key: a key made on this phone, as Connections → Dashboard Bluetooth
 * makes one, shown so it can be kept before it is saved on the bike.
 * @param {{ value: TextState, edited: boolean }} draft
 */
function KeyMaker(draft) {
  return div(
    { class: "settings-form-fields" },
    () =>
      generatedKey.val
        ? div(
            div({ class: "settings-note ble-key-readout" }, generatedKey.val),
            label(
              { class: "settings-note settings-check" },
              input({
                type: "checkbox",
                checked: () => generatedKept.val,
                onchange: event => {
                  generatedKept.val = /** @type {HTMLInputElement} */ (event.target).checked;
                },
              }),
              " I kept a copy of this key. Every phone needs it after the restart."
            )
          )
        : div(),
    div(
      { class: "settings-actions" },
      button(
        {
          class: "settings-button",
          disabled: () => !!busy.val,
          onclick: () => {
            generatedKey.val = generateBleSecret();
            generatedKept.val = false;
            draft.value.val = generatedKey.rawVal;
            draft.edited = true;
          },
        },
        () => (generatedKey.val ? "Another key" : "New key")
      ),
      () =>
        generatedKey.val
          ? button(
              {
                class: "settings-button",
                onclick: () => {
                  void navigator.clipboard?.writeText(generatedKey.rawVal).then(
                    () => {
                      results.val = { ...results.rawVal, BLE_SECRET: "Copied. Keep it somewhere safe, then Save." };
                    },
                    () => {
                      results.val = { ...results.rawVal, BLE_SECRET: "Copy it by hand from above, then Save." };
                    }
                  );
                },
              },
              "Copy"
            )
          : div({ hidden: true })
    )
  );
}

/** @param {ConsoleSetting} setting @param {{ value: TextState, edited: boolean }} draft */
async function save(setting, draft) {
  const value = draft.value.rawVal;
  const line =
    setting.name === "STATUS_REPORT"
      ? `status ${value === "1" ? "on" : "off"}`
      : setting.name === "STATUS_INTERVAL_S"
        ? `status every ${value}`
        : `set ${setting.name} ${quoted(value)}`;
  const reply = await runLine(setting.name, line);
  if (!reply?.ok) return;
  draft.edited = false;
  if (setting.type === "secret") draft.value.val = "";
  if (setting.name === "BLE_SECRET") {
    // Restart asks for the new key; this phone already has it.
    restartKey.val = canonicalBleSecret(value);
    generatedKey.val = "";
    generatedKept.val = false;
  }
}

/** @param {ConsoleSetting} setting @param {{ value: TextState, edited: boolean }} draft */
function Editor(setting, draft) {
  const onInput = /** @param {Event} event */ event => {
    draft.value.val = /** @type {HTMLInputElement | HTMLSelectElement} */ (event.target).value;
    draft.edited = true;
  };
  if (setting.type === "flag") {
    return select(
      { "aria-label": setting.name, value: () => draft.value.val, onchange: onInput },
      option({ value: "0", selected: () => draft.value.val === "0" }, "0 — off"),
      option({ value: "1", selected: () => draft.value.val === "1" }, "1 — on")
    );
  }
  if (setting.type === "integer") {
    return input({
      "aria-label": setting.name,
      type: "number",
      inputmode: "numeric",
      min: setting.minimum,
      max: setting.maximum,
      step: 1,
      value: () => draft.value.val,
      oninput: onInput,
    });
  }
  return input({
    "aria-label": setting.name,
    type: setting.type === "secret" ? "password" : "text",
    autocomplete: setting.type === "secret" ? "new-password" : "off",
    spellcheck: false,
    placeholder:
      setting.type === "secret"
        ? `${setting.saved}; type a new one to replace it`
        : setting.defaultValue
          ? `default: ${setting.defaultValue}`
          : "",
    maxlength: setting.maximum,
    value: () => draft.value.val,
    oninput: onInput,
  });
}

function CommandCard() {
  return SettingsCard(
    "Command line",
    "Type a command as at the emcool> prompt: help, show, status, network, set NAME VALUE, unset NAME, and the card's dir, cd, type, mkdir, rm, cp and mv. Quote a value with spaces.",
    div(
      { class: "settings-row" },
      div(
        input({
          "aria-label": "Console command",
          spellcheck: false,
          autocomplete: "off",
          autocapitalize: "off",
          placeholder: "show",
          value: () => commandLine.val,
          oninput: event => {
            commandLine.val = /** @type {HTMLInputElement} */ (event.target).value;
          },
          onkeydown: event => {
            if (event.key === "Enter" && !event.repeat) {
              event.preventDefault();
              void submitCommand();
            }
          },
        })
      ),
      button(
        {
          class: "settings-button",
          disabled: () => !!busy.val || !linkReady() || !commandLine.val.trim(),
          onclick: () => {
            void submitCommand();
          },
        },
        "Run"
      )
    ),
    () =>
      transcript.val.length
        ? pre(
            { style: OUTPUT_STYLE },
            ...transcript.val.flatMap(entry => [
              span({ style: "color: var(--sub);" }, `emcool> ${entry.line}\n`),
              entry.output.endsWith("\n") || !entry.output ? entry.output : `${entry.output}\n`,
            ])
          )
        : div()
  );
}

async function submitCommand() {
  const line = commandLine.rawVal.trim();
  if (!line) return;
  commandLine.val = "";
  // These go through their cards, which ask twice (and Restart checks a pending key), never straight from here.
  const refusal = /^restart\b/i.test(line)
    ? "Use Restart above: it asks twice."
    : /^set\s+(SERVICE_WRITE_ENABLED|CHARGE_AUTO_ENABLED)\s+("?)(1|on|true|yes)\2\s*$/i.test(line)
      ? "Turn this on from its own card above: it asks twice."
      : "";
  if (refusal) {
    transcript.val = [...transcript.rawVal, { line, output: `${refusal}\n` }].slice(-12);
    return;
  }
  await runLine("command", line);
}
