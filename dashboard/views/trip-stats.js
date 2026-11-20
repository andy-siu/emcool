// @ts-check

import van from "../vendor/van-1.6.1.js";
import { chartTick, valueOf } from "../lib/store.js";
import { averageMovingSpeedKmh, distanceKm, movingTimeSeconds, topSpeed } from "../lib/trip.js";
import { compass, duration } from "../lib/format.js";
import * as units from "../lib/units.js";

const { div } = van.tags;

// The "This session" grid: what this ride has done so far. Read-only, and kept apart from
// views/sheet.js, which is otherwise controls.

export function TripStats() {
  return div(
    { class: "stats" },
    Stat("Distance", () => {
      chartTick.val;
      const travelledKm = distanceKm();
      return travelledKm == null ? "–" : `${units.distance(travelledKm).toFixed(1)} ${units.distanceUnit()}`;
    }),
    Stat("Moving", () => {
      chartTick.val;
      return duration(movingTimeSeconds());
    }),
    Stat("Average", () => {
      chartTick.val;
      const average = averageMovingSpeedKmh();
      return average == null ? "–" : `${units.speed(average).toFixed(0)} ${units.speedUnit()}`;
    }),
    Stat("Top", () => {
      chartTick.val;
      return `${units.speed(topSpeed()).toFixed(0)} ${units.speedUnit()}`;
    }),
    Stat("Altitude", () => {
      const metres = valueOf("gps_altitude_m");
      return metres == null ? "–" : `${Math.round(units.altitude(metres))} ${units.altitudeUnit()}`;
    }),
    Stat("Heading", () => compass(valueOf("gps_course_deg"))),
    Stat("Satellites", () => {
      const satellites = valueOf("gps_satellites");
      return satellites == null ? "–" : String(Math.round(satellites));
    })
  );
}

/**
 * @param {string} label
 * @param {() => string} value
 */
function Stat(label, value) {
  return div(
    { class: "stat" },
    div({ class: "stat-label" }, label),
    div({ class: "stat-value" }, value)
  );
}
