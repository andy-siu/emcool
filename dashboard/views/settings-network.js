// @ts-check
// Settings → Network connections: the Wi-Fi network the bike joins (WIFI_SSID, WIFI_PASSWORD) and
// the admin password (ADMIN_PASSWORD) that the telnet console and the ride-log page ask for.
//
// It saves with the console's own `set` and `unset`, through the Bike console's runLine(), so the
// bike checks every value exactly as at the emcool> prompt (docs/wifi.md). The one rule added here:
// Wi-Fi runs only with both WIFI_SSID and ADMIN_PASSWORD set (Settings::wifiEnabled()), so this page
// won't save a network until an admin password is saved or typed beside it. The admin password is
// saved first and the network last, so a save that stops halfway never leaves a network saved
// without its admin password.
import van from "../vendor/van-1.6.1.js";
import { SettingsCard } from "./settings-fields.js";
import { ConsoleRestart, busy, linkReady, list, listError, quoted, results, runLine, watchConsoleList } from "./bike-console.js";

const { button, div, input, label, pre } = van.tags;

const ssid = van.state("");
const ssidEdited = van.state(false);
const wifiPassword = van.state("");
const openNetwork = van.state(false);
const adminPassword = van.state("");
const showPasswords = van.state(false);
const ifconfig = van.state("");

const RESULT = "network";
const OUTPUT_STYLE =
  "font-family: ui-monospace, Menlo, monospace; font-size: 12px; line-height: 1.45; white-space: pre-wrap; overflow-wrap: anywhere; margin: 10px 0 0; color: var(--fg);";

/** UTF-8 bytes, which is what the bike counts. @param {string} text */
const byteLength = text => new TextEncoder().encode(text).length;

/** @param {string} name */
const setting = name => list.val?.settings.find(entry => entry.name === name);

/** The SSID field: what was typed, else what is saved. */
const ssidValue = () => (ssidEdited.val ? ssid.val : (setting("WIFI_SSID")?.saved ?? ""));

/**
 * Why Save can't run yet, or "" when it can. Mirrors the bike's own checks (settings.cpp validate()),
 * so the reason shows before the tap rather than after it.
 */
function blocker() {
  const current = list.val;
  if (!linkReady()) return "Connect over Bluetooth to change the network settings.";
  if (!current) return listError.val || "Reading the bike's settings…";
  const name = ssidValue();
  const nameBytes = byteLength(name);
  const savedSsid = setting("WIFI_SSID")?.saved ?? "";
  const adminSaved = setting("ADMIN_PASSWORD")?.set === true;
  const admin = adminPassword.val;
  const password = wifiPassword.val;
  if (nameBytes > 32) return `The network name is ${nameBytes} bytes; it can be at most 32.`;
  const adminBytes = byteLength(admin);
  if (admin && (adminBytes < 8 || adminBytes > 128)) {
    return `The admin password must be 8-128 characters; this is ${adminBytes}.`;
  }
  const passwordBytes = byteLength(password);
  if (!openNetwork.val && password && (passwordBytes < 8 || passwordBytes > 63)) {
    return `The Wi-Fi password must be 8-63 characters; this is ${passwordBytes}.`;
  }
  if (ssidEdited.val && !name && savedSsid) return "To turn Wi-Fi off, tap Turn Wi-Fi off.";
  if (name && !adminSaved && !admin) {
    return "Enter an admin password. Wi-Fi stays off without one, because it guards the telnet console and the ride-log page.";
  }
  const changes =
    (ssidEdited.val && name && name !== savedSsid) || admin || password || (openNetwork.val && setting("WIFI_PASSWORD")?.set);
  if (!changes) return "Nothing to save yet.";
  return "";
}

/** Runs one line, and stops the save at the first refusal. @param {string} line */
async function step(line) {
  const reply = await runLine(RESULT, line);
  return reply?.ok === true;
}

async function save() {
  const name = ssidValue();
  const savedSsid = setting("WIFI_SSID")?.saved ?? "";
  const admin = adminPassword.rawVal;
  const password = wifiPassword.rawVal;
  const outputs = [];
  if (admin) {
    if (!(await step(`set ADMIN_PASSWORD ${quoted(admin)}`))) return;
    outputs.push(results.rawVal[RESULT]);
    adminPassword.val = "";
  }
  if (openNetwork.rawVal) {
    if (setting("WIFI_PASSWORD")?.set) {
      if (!(await step("unset WIFI_PASSWORD"))) return;
      outputs.push(results.rawVal[RESULT]);
    }
    openNetwork.val = false;
  } else if (password) {
    if (!(await step(`set WIFI_PASSWORD ${quoted(password)}`))) return;
    outputs.push(results.rawVal[RESULT]);
    wifiPassword.val = "";
  }
  if (name && name !== savedSsid) {
    if (!(await step(`set WIFI_SSID ${quoted(name)}`))) return;
    outputs.push(results.rawVal[RESULT]);
  }
  ssidEdited.val = false;
  results.val = {
    ...results.rawVal,
    [RESULT]: `${outputs.join("\n")}\nSaved. Restart emcool below for it to take effect.`.trim(),
  };
}

async function turnOff() {
  if (await step("unset WIFI_SSID")) {
    ssidEdited.val = false;
    ssid.val = "";
  }
}

async function readIfconfig() {
  const reply = await runLine("network-ifconfig", "ifconfig");
  if (reply?.ok) ifconfig.val = reply.output;
}

/** @param {string} name */
function savedLine(name) {
  const entry = setting(name);
  if (!entry) return "";
  return entry.pending ? `${entry.saved} (running now: ${entry.active}, until Restart)` : entry.saved;
}

function Summary() {
  return () => {
    if (!list.val) return div();
    const name = setting("WIFI_SSID");
    const admin = setting("ADMIN_PASSWORD");
    const savedOn = !!name?.set && !!admin?.set;
    const runningOn = !!name?.active && !!admin?.active && admin.active !== "not set";
    return div(
      { class: "settings-note settings-state" },
      div(`Wi-Fi: ${runningOn ? "on" : "off"} now${savedOn === runningOn ? "" : `; ${savedOn ? "on" : "off"} after Restart`}.`),
      div(`Network: ${name?.set ? savedLine("WIFI_SSID") : "not set"}.`),
      div(`Wi-Fi password: ${savedLine("WIFI_PASSWORD")}.`),
      div(`Admin password: ${savedLine("ADMIN_PASSWORD")}.`)
    );
  };
}

/** @param {string} ariaLabel @param {import("../vendor/van-1.6.1.js").State<string>} value @param {() => string} placeholder @param {() => boolean} [disabled] */
function PasswordField(ariaLabel, value, placeholder, disabled = () => false) {
  return input({
    "aria-label": ariaLabel,
    type: () => (showPasswords.val ? "text" : "password"),
    autocomplete: "new-password",
    autocapitalize: "off",
    spellcheck: false,
    placeholder,
    maxlength: 128,
    disabled,
    value: () => value.val,
    oninput: event => {
      value.val = /** @type {HTMLInputElement} */ (event.target).value;
    },
  });
}

/** @param {string} text @param {import("../vendor/van-1.6.1.js").ChildDom} field */
function Field(text, field) {
  return label({ class: "settings-form-fields" }, div({ class: "settings-note" }, text), field);
}

export function NetworkConnections() {
  watchConsoleList();
  return div(
    SettingsCard(
      "Wi-Fi",
      "The bike joins this network (garage Wi-Fi or a phone hotspot) and serves the telnet console on port 23 and the ride-log page on port 80, both behind the admin password. It needs a network name and an admin password; the change takes effect after Restart. ⚠️ Both passwords and everything after them cross the network in plain text, so use a network you control.",
      () =>
        div(
          { class: "settings-note", role: "status" },
          linkReady() ? listError.val : "Connect over Bluetooth to change the network settings."
        ),
      Summary()
    ),
    SettingsCard(
      "Change the network",
      "Leave a password blank to keep the one saved. Passwords are never sent back, only whether they are set.",
      Field(
        "Network name (SSID), 1-32 characters",
        input({
          "aria-label": "Network name (SSID)",
          type: "text",
          autocomplete: "off",
          autocapitalize: "off",
          spellcheck: false,
          maxlength: 32,
          placeholder: "garage wifi",
          value: () => ssidValue(),
          oninput: event => {
            ssid.val = /** @type {HTMLInputElement} */ (event.target).value;
            ssidEdited.val = true;
          },
        })
      ),
      Field(
        "Wi-Fi password, 8-63 characters",
        PasswordField(
          "Wi-Fi password",
          wifiPassword,
          () => (setting("WIFI_PASSWORD")?.set ? "saved; type a new one to replace it" : "not set"),
          () => openNetwork.val
        )
      ),
      label(
        { class: "settings-note settings-check" },
        input({
          type: "checkbox",
          checked: () => openNetwork.val,
          onchange: event => {
            openNetwork.val = /** @type {HTMLInputElement} */ (event.target).checked;
            if (openNetwork.rawVal) wifiPassword.val = "";
          },
        }),
        " Open network (no Wi-Fi password)"
      ),
      Field(
        "Admin password, 8-128 characters. Required to turn Wi-Fi on. Use one you use nowhere else, not the Bluetooth key.",
        PasswordField(
          "Admin password",
          adminPassword,
          () => (setting("ADMIN_PASSWORD")?.set ? "saved; type a new one to replace it" : "required to turn Wi-Fi on")
        )
      ),
      label(
        { class: "settings-note settings-check" },
        input({
          type: "checkbox",
          checked: () => showPasswords.val,
          onchange: event => {
            showPasswords.val = /** @type {HTMLInputElement} */ (event.target).checked;
          },
        }),
        " Show passwords"
      ),
      () => {
        const why = blocker();
        return why ? div({ class: "settings-note", role: "status" }, why) : div();
      },
      div(
        { class: "settings-actions" },
        button(
          {
            class: "settings-button",
            disabled: () => !!busy.val || !!blocker(),
            onclick: () => {
              void save();
            },
          },
          () => (busy.val === RESULT ? "Saving…" : "Save")
        ),
        () =>
          setting("WIFI_SSID")?.set
            ? button(
                {
                  class: "settings-button",
                  disabled: () => !!busy.val || !linkReady(),
                  onclick: () => {
                    void turnOff();
                  },
                },
                "Turn Wi-Fi off"
              )
            : div({ hidden: true })
      ),
      () => (results.val[RESULT] ? pre({ style: OUTPUT_STYLE }, results.val[RESULT]) : div())
    ),
    SettingsCard(
      "Network status",
      "The ifconfig report: whether the bike has joined, its address, and where to find the console and the ride-log page.",
      div(
        { class: "settings-actions" },
        button(
          {
            class: "settings-button",
            disabled: () => !!busy.val || !linkReady() || !list.val,
            onclick: () => {
              void readIfconfig();
            },
          },
          () => (busy.val === "network-ifconfig" ? "Reading…" : ifconfig.val ? "Read again" : "Read status")
        )
      ),
      () =>
        ifconfig.val
          ? pre({ style: OUTPUT_STYLE }, ifconfig.val)
          : div({ class: "settings-note", role: "status" }, results.val["network-ifconfig"] ?? "")
    ),
    ConsoleRestart()
  );
}
