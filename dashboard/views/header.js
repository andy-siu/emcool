// @ts-check
// Header geometry, notice policy and CAN liveness constraints: docs/dashboard-header.md.
import van from "../vendor/van-1.6.1.js";
import { chartTick, connection, isStaleSampled, signalState, systemStatus } from "../lib/store.js";
import { destination, backDestinationName, openDestination, closeDestination } from "../lib/destination.js";
import { openSheet } from "./sheet.js";
import { BleHeaderButton } from "./ble-connect.js";
import { HeaderTitle } from "./header-title.js";

const { button, div, span } = van.tags;

export function Header() {
  return div(
    { class: () => (destination.val ? "header destination-header" : "header") },
    button(
      {
        class: "menu",
        onclick: () => (destination.rawVal ? closeDestination() : openSheet()),
        "aria-label": () => (destination.val ? `Back to ${backDestinationName.val}` : "Open menu"),
      },
      () => (destination.val ? `‹ ${backDestinationName.val}` : "☰")
    ),
    HeaderTitle(),
    div(
      { class: "status" },
      BleHeaderButton(() => openDestination("settings", "connections")),
      button(
        {
          class: "can-status-link",
          "aria-label": "CAN bus details",
          onclick: () => openDestination("settings", "connections"),
        },
        span({ class: () => `dot ${canDotState()}` }),
        "CAN"
      ),
      button(
        {
          class: "can-status-link",
          "aria-label": () => `Wi-Fi: ${WIFI_WORDS[wifiDotState()]}. Network info`,
          onclick: () => openDestination("network"),
        },
        span({ class: () => `dot ${wifiDotState()}` }),
        "Wi-Fi"
      ),
      span({ class: () => `dot ${connection.val}` }),
      span(() => (connection.val === "live" ? "live" : connection.val === "connecting" ? "connecting" : "offline"))
    )
  );
}

/**
 * emcool sends no `system` block: it records `can_link` once a second instead (1 while the bike
 * has been heard in the last 15 s and the controller isn't bus-off, 0 after that). A 1 counts only
 * while it is fresh, so a value left over from before the bike went quiet can't keep the dot green.
 */
const CAN_LINK_FRESH_MS = 15_000;

/** @returns {"up" | "down" | "unknown"} */
function canDotState() {
  if (connection.val !== "live") return "unknown";
  if (systemStatus.val) return systemStatus.val.can.state;
  // Paced by the chart tick, not every message: freshness needs a clock that keeps moving.
  chartTick.val;
  const link = signalState("can_link").val?.value;
  if (link === 0) return "down";
  if (link === 1 && !isStaleSampled("can_link", CAN_LINK_FRESH_MS)) return "up";
  return "unknown";
}

/**
 * The bike records `wifi_link_state` once a second beside `can_link`, in the reference's codes: 0 off
 * (WIFI_ENABLED 0, or no network set), 1 not joined (retrying), 2 joining, 3 joined. Grey when off or
 * unheard, amber while it is trying, green once it has an address, red when it can't join.
 */
const WIFI_STATE_FRESH_MS = 15_000;

/** @type {Record<"up" | "connecting" | "down" | "unknown", string>} */
const WIFI_WORDS = { up: "joined", connecting: "joining", down: "not joined", unknown: "off" };

/** @returns {"up" | "connecting" | "down" | "unknown"} */
function wifiDotState() {
  if (connection.val !== "live") return "unknown";
  chartTick.val;
  const value = signalState("wifi_link_state").val?.value;
  if (value === undefined || isStaleSampled("wifi_link_state", WIFI_STATE_FRESH_MS)) return "unknown";
  if (value === 3) return "up";
  if (value === 2) return "connecting";
  if (value === 1) return "down";
  return "unknown";
}
