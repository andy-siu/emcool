// @ts-check
// Settings → Network connections: the Wi-Fi network the bike joins (WIFI_SSID, WIFI_PASSWORD), the
// SSH console's password (ADMIN_PASSWORD), and the telnet password (TELNET_PASSWORD) that the
// read-only telnet console and the ride-log page ask for.
//
// It saves with the console's own `set` and `unset`, through the Bike console's runLine(), so the
// bike checks every value exactly as at the emcool> prompt (docs/wifi.md). The one rule added here:
// Wi-Fi runs only with WIFI_SSID and at least one of the passwords set (Settings::wifiEnabled()), so
// this page won't save a network until one is saved or typed beside it. The passwords are saved first
// and the network last, so a save that stops halfway never leaves a network saved without one. The
// two must differ (the bike refuses a match): the telnet one crosses the network in plain text.
import van from "../vendor/van-1.6.1.js";
import { SettingsCard } from "./settings-fields.js";
import { ConsoleRestart, busy, linkReady, list, listError, quoted, results, runLine, watchConsoleList } from "./bike-console.js";

const { button, div, input, label, pre } = van.tags;

const ssid = van.state("");
const ssidEdited = van.state(false);
const wifiPassword = van.state("");
const openNetwork = van.state(false);
const adminPassword = van.state("");
const telnetPassword = van.state("");
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
  const telnetSaved = setting("TELNET_PASSWORD")?.set === true;
  const admin = adminPassword.val;
  const telnet = telnetPassword.val;
  const password = wifiPassword.val;
  if (nameBytes > 32) return `The network name is ${nameBytes} bytes; it can be at most 32.`;
  const adminBytes = byteLength(admin);
  if (admin && (adminBytes < 8 || adminBytes > 128)) {
    return `The SSH password must be 8-128 characters; this is ${adminBytes}.`;
  }
  const telnetBytes = byteLength(telnet);
  if (telnet && (telnetBytes < 8 || telnetBytes > 128)) {
    return `The telnet password must be 8-128 characters; this is ${telnetBytes}.`;
  }
  if (admin && telnet && admin === telnet) {
    return "The SSH and telnet passwords must differ: the telnet one crosses the network in plain text.";
  }
  const passwordBytes = byteLength(password);
  if (!openNetwork.val && password && (passwordBytes < 8 || passwordBytes > 63)) {
    return `The Wi-Fi password must be 8-63 characters; this is ${passwordBytes}.`;
  }
  if (ssidEdited.val && !name && savedSsid) return "To turn Wi-Fi off, tap Turn Wi-Fi off.";
  if (name && !adminSaved && !admin && !telnetSaved && !telnet) {
    return "Enter an SSH password or a telnet password. Wi-Fi stays off without one, because they guard what it serves.";
  }
  const changes =
    (ssidEdited.val && name && name !== savedSsid) ||
    admin ||
    telnet ||
    password ||
    (openNetwork.val && setting("WIFI_PASSWORD")?.set);
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
  const telnet = telnetPassword.rawVal;
  const password = wifiPassword.rawVal;
  const outputs = [];
  if (admin) {
    if (!(await step(`set ADMIN_PASSWORD ${quoted(admin)}`))) return;
    outputs.push(results.rawVal[RESULT]);
    adminPassword.val = "";
  }
  if (telnet) {
    if (!(await step(`set TELNET_PASSWORD ${quoted(telnet)}`))) return;
    outputs.push(results.rawVal[RESULT]);
    telnetPassword.val = "";
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
    const telnet = setting("TELNET_PASSWORD");
    const isSet = entry => !!entry?.active && entry.active !== "not set";
    const savedOn = !!name?.set && (!!admin?.set || !!telnet?.set);
    const runningOn = isSet(name) && (isSet(admin) || isSet(telnet));
    return div(
      { class: "settings-note settings-state" },
      div(`Wi-Fi: ${runningOn ? "on" : "off"} now${savedOn === runningOn ? "" : `; ${savedOn ? "on" : "off"} after Restart`}.`),
      div(`Network: ${name?.set ? savedLine("WIFI_SSID") : "not set"}.`),
      div(`Wi-Fi password: ${savedLine("WIFI_PASSWORD")}.`),
      div(`SSH password: ${savedLine("ADMIN_PASSWORD")}.`),
      div(`Telnet password: ${savedLine("TELNET_PASSWORD")}.`)
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
      "The bike joins this network (garage Wi-Fi or a phone hotspot) and serves the SSH console on port 22 (ssh admin@<bike>, behind the SSH password), which runs every command, and the ride-log page on port 80 behind the telnet password. The read-only telnet console on port 23 is off unless TELNET_CONSOLE is 1. It needs a network name and at least one password; the change takes effect after Restart. ⚠️ The telnet password and the ride log cross the network in plain text; SSH is encrypted.",
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
        "SSH password (ADMIN_PASSWORD), 8-128 characters. The SSH console runs everything: settings, restart, firmware, the CAN commands. Use one you use nowhere else.",
        PasswordField(
          "SSH password",
          adminPassword,
          () => (setting("ADMIN_PASSWORD")?.set ? "saved; type a new one to replace it" : "not set: SSH is off")
        )
      ),
      Field(
        "Telnet password (TELNET_PASSWORD), 8-128 characters. For the ride-log page and the read-only telnet console; it crosses the network in plain text, so never the SSH password.",
        PasswordField(
          "Telnet password",
          telnetPassword,
          () => (setting("TELNET_PASSWORD")?.set ? "saved; type a new one to replace it" : "not set: the ride-log page is off")
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
      "The ifconfig report: whether the bike has joined, its address, the SSH host key's fingerprint, and where to find the consoles and the ride-log page.",
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
