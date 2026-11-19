// @ts-check

import van from "../vendor/van-1.6.1.js";
import { arm, armDwellElapsed, armed, refuseKeyRepeat } from "../lib/arming.js";
import { request } from "../lib/transport.js";

const { button, div } = van.tags;

// The control that MAINTAINS THE BIKE'S COMPUTER rather than the bike: its CAN interface. Split
// out of ./sheet.js when it gained the arming gate. (Firmware updates are ./firmware-update.js.)
//
// ⚠️ Not "the controls that act on emcool": the waypoint and the ride-log download act on
// emcool too (they write and read its log) and deliberately stayed behind. The line is
// maintenance of the machine, not use of it.
//
// Grey-tier `.action`, not amber `.action.writes`: amber on this sheet means
// "this touches the bike", and this doesn't. Why they arm anyway, and why the
// tier did not move with the tap count: docs/dashboard-decisions.md § "The menu sheet".

/** @typedef {import("../../src/http/can-restart.ts").CanRestartReply} CanRestartReply */

/**
 * ⚠️ Their own keys, in the `<module>:<control>` spelling views/service-mode.js uses.
 * `armed` is ONE key for the whole dashboard, and every firing site tests its own before
 * it acts — which is only safe while no two controls share one. scripts/check-arming.ts §5.
 */
const CAN_RESTART_KEY = "pi-actions:can-restart";

const canRestartMessage = van.state("");
const canRestartFailed = van.state(false);
const canRestarting = van.state(false);

/**
 * Re-ups can0 when the CAN dot has gone red. POSTs to /can-restart, which runs the two
 * `ip link` commands on emcool; the result note reports what happened, since the bus
 * coming back is not something this button can see from here — the CAN dot in the header
 * is what confirms it a poll later.
 *
 * Styled from `ok`: /can-restart answers 500 with
 * ok:false, and a failure rendered in the same grey as a success is the bug style.css
 * argues against for .action-note.failure.
 */
export function CanRestartButton() {
  return div(
    { class: "settings-action" },
    div(
      { class: "settings-row" },
      div({ class: "settings-note" }, "CAN link goes down and back up. Other CAN sockets close."),
      button(
        {
          class: "settings-button",
          disabled: canRestarting,
          // One held Enter must not arm and then fire. See ../lib/arming.js.
          onkeydown: refuseKeyRepeat,
          onclick: () => {
            if (armed.val !== CAN_RESTART_KEY) {
              arm(CAN_RESTART_KEY);
              return;
            }
            if (!armDwellElapsed()) {
              return;
            }
            armed.val = "";
            void performCanRestart();
          },
        },
        () => (armed.val === CAN_RESTART_KEY ? "Confirm restart" : "Restart CAN")
      )
    ),
    () =>
      canRestartMessage.val
        ? div({ class: `action-note${canRestartFailed.val ? " failure" : ""}` }, canRestartMessage.val)
        : div()
  );
}

/**
 * The CAN restart's firing site. Named for the `perform…` convention scripts/check-arming.ts
 * finds a firing site by, and module-level so its inputs are visible at a glance.
 *
 * ⚠️ The header is a CSRF barrier, not authentication: without it this POST is a SIMPLE
 * cross-origin request and any page the phone opens on the hotspot can drop the bus.
 * src/http/can-restart.ts holds the other end; both are literals, because neither file
 * can see the other's constant.
 */
async function performCanRestart() {
  canRestarting.val = true;
  canRestartFailed.val = false;
  canRestartMessage.val = "restarting…";
  try {
    const response = await request("/can-restart", { method: "POST", headers: { "X-Emcool": "can-restart" } });
    const reply = /** @type {CanRestartReply} */ (await response.json());
    canRestartMessage.val = reply.message;
    canRestartFailed.val = !reply.ok;
  } catch (error) {
    console.warn("can-restart: request failed", error);
    canRestartMessage.val = "Restart request failed — is emcool reachable?";
    canRestartFailed.val = true;
  } finally {
    canRestarting.val = false;
  }
}
