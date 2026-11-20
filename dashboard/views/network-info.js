// @ts-check
// Menu → Network info (also the header's Wi-Fi dot): what the bike's Wi-Fi is set to and what it is
// doing. The saved network and which passwords are set (from GET /console's settings list), an on/off
// button (WIFI_ENABLED, which applies at once), and the ifconfig report (whether it has joined, its
// address, the SSH host key's fingerprint, where the consoles and the ride-log page are), read when the
// page opens and again when the link comes back. Changing the network or the passwords is Settings →
// Network connections (./settings-network.js), which shows these same cards.
import van from "../vendor/van-1.6.1.js";
import { request } from "../lib/transport.js";
import { connection } from "../lib/store.js";
import { BLE_MODE } from "../lib/ble-link.js";
import { destination, openDestination } from "../lib/destination.js";
import { SettingsCard } from "./settings-fields.js";
import { OUTPUT_STYLE, busy, linkReady, list, listError, results, runLine, watchConsoleList } from "./bike-console.js";

const { button, div, h2, pre } = van.tags;

const report = van.state("");
const reportError = van.state("");
const reading = van.state(false);

/** @param {string} name */
const setting = name => list.val?.settings.find(entry => entry.name === name);

/**
 * Runs `ifconfig` on the bike's console. Called from inside bindings (the page opening), so it
 * reads states with rawVal: a .val here would re-draw the whole page on every change to them.
 */
async function readNetwork() {
  if (reading.rawVal || connection.rawVal !== "live" || !BLE_MODE) return;
  reading.val = true;
  reportError.val = "";
  try {
    const response = await request("/console", {
      method: "POST",
      headers: { "X-Emcool": "console", "Content-Type": "application/json" },
      body: JSON.stringify({ line: "ifconfig" }),
    });
    if (response.status === 404) {
      reportError.val = "This firmware has no Bike console. Update emcool, or run ifconfig on the serial console.";
      return;
    }
    const reply = /** @type {{ ok: boolean, output?: string }} */ (await response.json());
    const output = (reply.output ?? "").trim();
    if (reply.ok) {
      report.val = output;
    } else {
      reportError.val = output || `The bike answered ${response.status}.`;
    }
  } catch (error) {
    reportError.val = `Not read: ${error instanceof Error ? error.message : "no answer"}.`;
  } finally {
    reading.val = false;
  }
}

// Made here, outside any binding, so it outlives the page; opening the page reads it too.
van.derive(() => {
  if (connection.val === "live" && destination.rawVal === "network") void readNetwork();
});

/** @param {string} name */
function savedLine(name) {
  const entry = setting(name);
  if (!entry) return "";
  return entry.pending ? `${entry.saved} (running now: ${entry.active}, until Restart)` : entry.saved;
}

/** Whether Wi-Fi is on, now and after Restart, the network, and which passwords are set. */
export function NetworkSummary() {
  return () => {
    if (!list.val) return div();
    const name = setting("WIFI_SSID");
    const admin = setting("ADMIN_PASSWORD");
    const telnet = setting("TELNET_PASSWORD");
    const isSet = (/** @type {import("./bike-console.js").ConsoleSetting | undefined} */ entry) => !!entry?.active && entry.active !== "not set";
    const switchedOn = setting("WIFI_ENABLED")?.active !== "0";
    const savedOn = !!name?.set && (!!admin?.set || !!telnet?.set);
    const runningOn = switchedOn && isSet(name) && (isSet(admin) || isSet(telnet));
    const afterRestart = switchedOn && savedOn;
    return div(
      { class: "settings-note settings-state" },
      div(
        `Wi-Fi: ${runningOn ? "on" : "off"} now${!switchedOn ? " (turned off)" : ""}${afterRestart === runningOn || !switchedOn ? "" : `; ${afterRestart ? "on" : "off"} after Restart`}.`
      ),
      div(`Network: ${name?.set ? savedLine("WIFI_SSID") : "not set"}.`),
      div(`Wi-Fi password: ${savedLine("WIFI_PASSWORD")}.`),
      div(`SSH password: ${savedLine("ADMIN_PASSWORD")}.`),
      div(`Telnet password: ${savedLine("TELNET_PASSWORD")}.`)
    );
  };
}

const SWITCH = "network-switch";
const switchResult = van.state("");

/**
 * Turns Wi-Fi on or off now with `set WIFI_ENABLED`, which the bike applies at once and keeps across
 * restarts. The network and the passwords stay as they are. Turning it off closes any SSH, telnet or
 * ride-log connection, so a console reached over Wi-Fi goes with it.
 * @param {boolean} on
 */
async function switchWifi(on) {
  const reply = await runLine(SWITCH, `set WIFI_ENABLED ${on ? 1 : 0}`);
  switchResult.val = (reply?.output ?? results.rawVal[SWITCH] ?? "").trim();
  if (reply?.ok) {
    report.val = "";
    // A join takes a few seconds; read where it got to once it has had them.
    setTimeout(() => void readNetwork(), on ? 5000 : 500);
  }
}

/** The on/off button, once the settings are read: what it would do, and why it can't if it can't. */
export function WifiSwitch() {
  return () => {
    if (!list.val) return div();
    const enabled = setting("WIFI_ENABLED");
    if (!enabled) {
      return div({ class: "settings-note" }, "This firmware can't turn Wi-Fi on and off without a restart. Update emcool.");
    }
    const on = enabled.active !== "0";
    const name = setting("WIFI_SSID");
    // What this boot runs with: a network, and a password for something it serves.
    const running = (/** @type {string} */ key) => (setting(key)?.active ?? "not set") !== "not set";
    const setUp = !!name?.active && (running("ADMIN_PASSWORD") || running("TELNET_PASSWORD"));
    return div(
      div(
        { class: "settings-row" },
        div(on ? `On${setUp ? `: joins “${name?.active}”` : ", but no network is set up"}` : "Off"),
        button(
          {
            class: "settings-button",
            disabled: () => !!busy.val || reading.val || !linkReady(),
            onclick: () => {
              void switchWifi(!on);
            },
          },
          () => (busy.val === SWITCH ? "Working…" : on ? "Turn Wi-Fi off" : "Turn Wi-Fi on")
        )
      ),
      div(
        { class: "settings-note" },
        on
          ? "Off closes SSH, telnet and the ride-log page at once, and leaves the radio to Bluetooth. The network and the passwords stay saved."
          : setUp
            ? "On joins the saved network now, with no restart."
            : "On does nothing until a network and a password are saved and the bike restarts: Change the network settings."
      ),
      () => (switchResult.val ? pre({ style: OUTPUT_STYLE }, switchResult.val) : div())
    );
  };
}

/** The ifconfig report, with a button to read it again. */
export function NetworkStatus() {
  return SettingsCard(
    "Network status",
    "The ifconfig report: whether the bike has joined, its address, the SSH host key's fingerprint, and where to find the consoles and the ride-log page.",
    div(
      { class: "settings-actions" },
      button(
        {
          class: "settings-button",
          // Not while Settings → Network connections is saving: its steps go first.
          disabled: () => reading.val || !!busy.val || !linkReady(),
          onclick: () => {
            void readNetwork();
          },
        },
        () => (reading.val ? "Reading…" : report.val ? "Read again" : "Read status")
      )
    ),
    () => div({ class: "settings-note", role: "status" }, linkReady() ? reportError.val : ""),
    () => (report.val ? pre({ style: OUTPUT_STYLE }, report.val) : div())
  );
}

export function NetworkInfo() {
  watchConsoleList();
  void readNetwork();
  return div(
    { class: "settings-page" },
    h2("Network info"),
    SettingsCard(
      "Wi-Fi",
      "What the bike joins, and which passwords guard what it serves. Passwords are never sent back, only whether they are set.",
      () =>
        div(
          { class: "settings-note", role: "status" },
          linkReady() ? listError.val : "Connect over Bluetooth to read the network settings."
        ),
      NetworkSummary()
    ),
    SettingsCard("Turn Wi-Fi on or off", "", WifiSwitch()),
    NetworkStatus(),
    button({ class: "menu-link", onclick: () => openDestination("settings", "network") }, "Change the network settings →")
  );
}
