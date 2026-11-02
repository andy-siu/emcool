// Drives cool-eva's own GpsClockGate (src/gps/clock-gate.ts) through randomized clock
// scenarios and writes every offer with the verdict it produced, for host/tests/check_clock.cpp
// to replay through the C++ port. Each run starts a fresh gate. The scenarios cover:
//
// - cold boots, where the ESP32 wakes at 1970 with no battery-backed clock
// - small and large drift
// - single outliers, and bursts of a consistently wrong time (the contested-anchor path)
// - GPS week-number rollovers below the floor
// - non-numbers, and long gaps between readings
//
// The system clock is stepped whenever a verdict says so, as the firmware would.
//
//   node tools/generate-golden-clock.mts <cool-eva checkout> <output file>
//
// Line format: `<run> <gps epoch s> <system epoch s> <monotonic ms> <step 0|1> <reason>
// [<step epoch s> <offset s>]`, numbers written by String().

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

interface Verdict {
  step: boolean;
  reason: string;
  epochSeconds?: number;
  offsetSeconds?: number;
}

const [coolEvaDirectory, outputPath] = process.argv.slice(2);
if (!coolEvaDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-clock.mts <cool-eva checkout> <output file>");
  process.exit(2);
}

const gateModule = await import(pathToFileURL(join(coolEvaDirectory, "src/gps/clock-gate.ts")).href);
const GpsClockGate = gateModule.GpsClockGate as new () => {
  offer(epochSeconds: number, systemEpochSeconds: number, monotonicMs: number): Verdict;
};

const random = mulberry32(0x0c10c4);
const lines: string[] = [];
const reasonsSeen = new Map<string, number>();
const RUNS = 400;
const OFFERS_PER_RUN = 220;
const FLOOR = 1767225600;

for (let run = 0; run < RUNS; run++) {
  const gate = new GpsClockGate();
  let monotonicMs = Math.floor(random() * 1_000_000);
  let trueEpoch = FLOOR + random() * 4 * 365 * 86400;
  let systemEpoch = initialSystemClock(trueEpoch, monotonicMs);
  let wrongBurstRemaining = 0;
  let wrongBurstOffset = 0;

  for (let offer = 0; offer < OFFERS_PER_RUN; offer++) {
    const elapsedMs = nextIntervalMs();
    monotonicMs += elapsedMs;
    trueEpoch += elapsedMs / 1000;
    // The system clock drifts slightly from monotonic time between steps, and now and then
    // jumps by minutes, as if something else had set it: what a "drift" step corrects.
    systemEpoch += (elapsedMs / 1000) * (1 + (random() - 0.5) * 1e-4);
    if (random() < 0.004) {
      systemEpoch += (random() < 0.5 ? -1 : 1) * (61 + random() * 3400);
    }

    if (wrongBurstRemaining === 0 && random() < 0.01) {
      wrongBurstRemaining = 5 + Math.floor(random() * 40);
      wrongBurstOffset = pick([3600, -86400, 15, 900, 7 * 86400]);
    }
    let gpsEpoch: number;
    if (wrongBurstRemaining > 0) {
      wrongBurstRemaining--;
      gpsEpoch = trueEpoch + wrongBurstOffset;
    } else {
      gpsEpoch = gpsReading(trueEpoch);
    }

    const verdict = gate.offer(gpsEpoch, systemEpoch, monotonicMs);
    reasonsSeen.set(verdict.reason, (reasonsSeen.get(verdict.reason) ?? 0) + 1);
    let line = `${run} ${String(gpsEpoch)} ${String(systemEpoch)} ${String(monotonicMs)} ${verdict.step ? 1 : 0} ${verdict.reason}`;
    if (verdict.step) {
      line += ` ${String(verdict.epochSeconds)} ${String(verdict.offsetSeconds)}`;
      // Most steps succeed; a few fail and leave the clock where it was.
      if (random() < 0.95) {
        systemEpoch = verdict.epochSeconds!;
      }
    }
    lines.push(line);
  }
}

await writeFile(outputPath, lines.join("\n") + "\n");
console.log(
  `golden-clock: ${lines.length} offers over ${RUNS} runs → ${outputPath} (` +
    [...reasonsSeen].map(([reason, count]) => `${reason} ${count}`).join(", ") +
    ")"
);
const expectedReasons = [
  "cold-boot", "drift", "clock-implausible", "not-a-time", "before-floor", "awaiting-corroboration",
  "inconsistent-readings", "disagrees-with-known-good", "in-agreement", "cooling-down",
];
const missing = expectedReasons.filter(reason => !reasonsSeen.has(reason));
if (missing.length > 0) {
  console.error(`golden-clock: the scenarios never produced ${missing.join(", ")}`);
  process.exit(1);
}

function initialSystemClock(trueEpoch: number, monotonicMs: number): number {
  const roll = random();
  if (roll < 0.5) {
    return monotonicMs / 1000;
  }
  if (roll < 0.7) {
    return trueEpoch + (random() - 0.5) * 100;
  }
  if (roll < 0.9) {
    return trueEpoch + (random() - 0.5) * 2 * 86400 * 30;
  }
  return FLOOR - random() * 86400 * 365;
}

function nextIntervalMs(): number {
  const roll = random();
  if (roll < 0.9) {
    return 500 + Math.floor(random() * 1000);
  }
  if (roll < 0.97) {
    return 5_000 + Math.floor(random() * 60_000);
  }
  return 300_000 + Math.floor(random() * 1_200_000);
}

function gpsReading(trueEpoch: number): number {
  const roll = random();
  if (roll < 0.005) {
    return Number.NaN;
  }
  if (roll < 0.012) {
    return trueEpoch - 1024 * 7 * 86400;
  }
  if (roll < 0.03) {
    return trueEpoch + (random() - 0.5) * 2 * pick([10, 3600, 86400 * 400]);
  }
  return Math.round((trueEpoch + (random() - 0.5) * 0.2) * 1000) / 1000;
}

function pick<T>(choices: readonly T[]): T {
  return choices[Math.floor(random() * choices.length)];
}

function mulberry32(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let mixed = state;
    mixed = Math.imul(mixed ^ (mixed >>> 15), mixed | 1);
    mixed ^= mixed + Math.imul(mixed ^ (mixed >>> 7), mixed | 61);
    return ((mixed ^ (mixed >>> 14)) >>> 0) / 4294967296;
  };
}
