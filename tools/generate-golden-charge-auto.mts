// Runs the reference's own automatic-charge decision code on random/scripted input and writes what it
// returned, for host/tests/check_charge_auto.cpp to hold the C++ port to: the control law
// (decideChargeCurrent) and its support (estimateHeatingRate, sessionAheadMinutes, estimateSocRate).
//
//   node tools/generate-golden-charge-auto.mts <reference checkout> <output file>
//
// Each line is `<kind> <fields…>`; JSON fields are JSON.stringify's.

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-charge-auto.mts <reference checkout> <output file>");
  process.exit(2);
}
const load = (path: string) => import(pathToFileURL(join(referenceDirectory, path)).href);
console.log = console.warn = console.error = () => {};

const curve = await load("src/charge/auto-curve.ts");
const rate = await load("src/charge/rate.ts");
const soc = await load("src/charge/soc.ts");
const fanCurve = await load("src/fan/curve.ts");

function mulberry32(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let m = state;
    m = Math.imul(m ^ (m >>> 15), m | 1);
    m ^= m + Math.imul(m ^ (m >>> 7), m | 61);
    return ((m ^ (m >>> 14)) >>> 0) / 4294967296;
  };
}
const random = mulberry32(0xca5e);
const chance = (p: number) => random() < p;
const between = (lo: number, hi: number) => lo + Math.floor(random() * (hi - lo + 1));

const lines: string[] = [];
const emit = (line: string) => lines.push(line);
const json = (value: unknown) => JSON.stringify(value);
const counts: Record<string, number> = {};
const count = (k: string) => (counts[k] = (counts[k] ?? 0) + 1);

// Build a plausible temperature history ending near `now`, warming/cooling/flat.
function temperatureHistory(nowMs: number): { atMs: number; celsius: number }[] {
  const samples: { atMs: number; celsius: number }[] = [];
  const points = between(0, 8);
  let celsius = between(20, 58);
  const trend = chance(0.4) ? 0 : (chance(0.5) ? 1 : -1) * (random() * 0.6);
  for (let at = points; at > 0; at--) {
    const atMs = nowMs - between(0, 700000);
    celsius += trend * (random() * 3);
    samples.push({ atMs, celsius: Math.round(celsius * (chance(0.3) ? 1 : 10)) / (chance(0.3) ? 1 : 10) });
  }
  samples.sort((a, b) => a.atMs - b.atMs);
  return samples;
}
function socHistory(nowMs: number): { atMs: number; percent: number }[] {
  const samples: { atMs: number; percent: number }[] = [];
  let percent = between(10, 99);
  for (let at = between(0, 8); at > 0; at--) {
    percent += random() * 0.5;
    samples.push({ atMs: nowMs - between(0, 700000), percent: Math.min(100, Math.round(percent * 10) / 10) });
  }
  samples.sort((a, b) => a.atMs - b.atMs);
  return samples;
}

// --- 1. estimateHeatingRate --------------------------------------------------------------------
for (let at = 0; at < 4000; at++) {
  const nowMs = between(700000, 5000000);
  const samples = temperatureHistory(nowMs);
  emit(`RATE ${nowMs} ${json(samples)} ${json(rate.estimateHeatingRate(samples, nowMs))}`);
  count("RATE");
}

// --- 2. estimateSocRate and sessionAheadMinutes ------------------------------------------------
for (let at = 0; at < 3000; at++) {
  const nowMs = between(700000, 5000000);
  const samples = socHistory(nowMs);
  emit(`SOCRATE ${nowMs} ${json(samples)} ${json(soc.estimateSocRate(samples, nowMs) ?? null)}`);
  count("SOCRATE");
  const input = {
    socPercent: chance(0.9) ? between(10, 100) : null,
    socAgeMs: chance(0.9) ? between(0, 8000) : null,
    socSamples: samples,
    requestedAmps: chance(0.9) ? between(30, 80) : null,
    nowMs,
  };
  emit(`AHEAD ${json(input)} ${json(soc.sessionAheadMinutes(input) ?? null)}`);
  count("AHEAD");
}

// --- 3. decideChargeCurrent --------------------------------------------------------------------
for (let at = 0; at < 8000; at++) {
  const nowMs = between(700000, 5000000);
  const samples = temperatureHistory(nowMs);
  const packTemperatureC = chance(0.9) ? between(10, 60) + (chance(0.5) ? random() : 0) : (chance(0.5) ? null : between(-50, 120));
  const input = {
    enabled: chance(0.95),
    packTemperatureC,
    packTemperatureAgeMs: chance(0.9) ? between(0, 8000) : null,
    packTemperaturePlausible: fanCurve.isPackTemperaturePlausible(packTemperatureC),
    chargeManagerState: chance(0.8) ? 0x23 : (chance(0.5) ? 0x02 : null),
    chargeManagerStateAgeMs: chance(0.9) ? between(0, 8000) : null,
    ceilingAmps: chance(0.9) ? between(30, 80) : null,
    commandedAmps: chance(0.6) ? between(35, 80) : null,
    riderOverride: chance(0.05),
    samples,
    socPercent: chance(0.8) ? between(10, 100) : null,
    socAgeMs: chance(0.9) ? between(0, 8000) : null,
    socSamples: socHistory(nowMs),
    requestedAmps: chance(0.8) ? between(30, 80) : null,
    lastCommandAtMs: chance(0.6) ? nowMs - between(0, 900000) : null,
    nowMs,
  };
  emit(`DECIDE ${json(input)} ${json(curve.decideChargeCurrent(input))}`);
  count("DECIDE");
}

await writeFile(outputPath, lines.join("\n") + "\n");
process.stderr.write(
  "golden-charge-auto: " +
    Object.entries(counts)
      .map(([k, n]) => `${k}=${n}`)
      .join(" ") +
    "\n"
);
