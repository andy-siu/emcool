// Drives the reference's own pack-temperature routing (src/can/pack-temperature.ts, behind
// decodeFrame) through scripted 0x200/0x660 sequences on a controlled clock, and writes each
// frame with the values it produced, and each run with how often it logged, for
// host/tests/check_pack_temperature.cpp to replay through the C++ port.
//
// Fixed scenarios, then randomized runs covering:
// - a stock pack, and the custom LiBAL config with CUSTOM_BMS_CONFIG on and off
// - short, long and changing 0x660s, including a 0x660 that starts late
// - echo mismatches, and clamp gates that aren't masks or are stuck open
//
//   node tools/generate-golden-pack-temperature.mts <reference checkout> <output file>
//
// Lines: `run <custom 0|1>`, then `<ms> <id hex> <payload hex> [<key>=<value>]...` per frame,
// then `logs <errors> <warnings> <infos>`.

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

interface DecodedValue {
  key: string;
  value: number;
}

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-pack-temperature.mts <reference checkout> <output file>");
  process.exit(2);
}

// The reference's monotonicNow() is performance.now(); the scenarios set it.
let nowMs = 0;
Object.defineProperty(performance, "now", { value: () => nowMs, configurable: true, writable: true });
const counts = { error: 0, warn: 0, log: 0 };
console.error = () => void counts.error++;
console.warn = () => void counts.warn++;
console.log = () => void counts.log++;
const report = (line: string) => process.stdout.write(line + "\n");

const decode = await import(pathToFileURL(join(referenceDirectory, "src/can/decode.ts")).href);
const packTemperature = await import(pathToFileURL(join(referenceDirectory, "src/can/pack-temperature.ts")).href);
const decodeFrame = decode.decodeFrame as (id: number, data: Buffer) => DecodedValue[];
const resolve = packTemperature.resolvePackTemperatures as (
  id: number,
  data: Buffer,
  decoded: DecodedValue[]
) => DecodedValue[];
const configure = packTemperature.configurePackTemperature as (custom: boolean) => void;

const random = mulberry32(0x0660);
const lines: string[] = [];
const totals = { runs: 0, frames: 0, trueKeys: 0, errors: 0, warnings: 0, infos: 0 };

interface Scenario {
  custom: boolean;
  durationMs: number;
  // When 0x660 frames start, if at all, and how each is built.
  thermalStartMs: number | null;
  thermalLength: (index: number) => number;
  clampGate: (index: number) => number;
  trueHigh: (index: number) => number;
  echoOffset: (index: number) => number;
  // 0x200 can start late too, so the 5 s wait opens after a 0x660 has been seen.
  vcuStartMs: number;
}

const fixed: Scenario[] = [
  base({ custom: false, thermalStartMs: null }),
  base({ custom: true, thermalStartMs: null }),
  base({ custom: true, thermalStartMs: 0 }),
  base({ custom: false, thermalStartMs: 0 }),
  base({ custom: false, thermalStartMs: 7000 }),
  base({ custom: true, thermalStartMs: 0, thermalLength: () => 3 }),
  base({ custom: false, thermalStartMs: 0, thermalLength: () => 3 }),
  base({ custom: true, thermalStartMs: 3800, thermalLength: () => 3 }),
  base({ custom: true, thermalStartMs: 0, thermalLength: index => (index < 4 ? 3 : 8) }),
  base({ custom: true, thermalStartMs: 0, thermalLength: index => (index < 4 ? 8 : 3) }),
  base({ custom: true, thermalStartMs: 0, thermalLength: index => [3, 8, 3, 3, 6, 5][index % 6] }),
  base({ custom: true, thermalStartMs: 0, echoOffset: () => 4 }),
  base({ custom: true, thermalStartMs: 0, echoOffset: index => (index % 2 === 0 ? 1 : 0) }),
  base({ custom: true, thermalStartMs: 0, clampGate: () => 254 }),
  base({ custom: true, thermalStartMs: 0, clampGate: () => 17 }),
  base({ custom: true, thermalStartMs: 0, clampGate: () => 0, trueHigh: () => 45 }),
  base({ custom: true, thermalStartMs: 0, clampGate: () => 0, trueHigh: () => 30 }),
  base({ custom: true, thermalStartMs: 0, clampGate: () => 255, trueHigh: () => 50 }),
  base({ custom: false, thermalStartMs: 0, vcuStartMs: 6000 }),
];

for (const scenario of fixed) {
  runScenario(scenario);
}
for (let run = 0; run < 400; run++) {
  const pick = <T,>(choices: readonly T[]) => choices[Math.floor(random() * choices.length)];
  const thermalLengths = pick([[3], [8], [5, 6, 7], [3, 8], [3, 3, 8], [8, 8, 3]]);
  const gates = pick([[255], [0], [255, 0], [254], [255, 0, 17]]);
  const echoMismatch = random() < 0.3;
  runScenario({
    custom: random() < 0.5,
    durationMs: 2000 + Math.floor(random() * 14000),
    thermalStartMs: random() < 0.3 ? null : Math.floor(random() * 9000),
    thermalLength: () => pick(thermalLengths),
    clampGate: () => pick(gates),
    trueHigh: () => 25 + Math.floor(random() * 40),
    echoOffset: () => (echoMismatch && random() < 0.8 ? 1 + Math.floor(random() * 5) : 0),
    vcuStartMs: random() < 0.8 ? 0 : Math.floor(random() * 8000),
  });
}

await writeFile(outputPath, lines.join("\n") + "\n");
report(
  `golden-pack-temperature: ${totals.runs} runs, ${totals.frames} frames, ${totals.trueKeys} true-temperature ` +
    `values from 0x200, ${totals.errors} errors / ${totals.warnings} warnings / ${totals.infos} infos logged → ${outputPath}`
);
for (const [what, count] of Object.entries({
  "batt_temp from 0x200": totals.trueKeys,
  errors: totals.errors,
  warnings: totals.warnings,
  infos: totals.infos,
})) {
  if (count === 0) {
    report(`golden-pack-temperature: the scenarios never produced any ${what}`);
    process.exit(1);
  }
}

function base(overrides: Partial<Scenario>): Scenario {
  return {
    custom: false,
    durationMs: 12000,
    thermalStartMs: null,
    thermalLength: () => 8,
    clampGate: () => 255,
    trueHigh: index => 30 + (index % 8),
    echoOffset: () => 0,
    vcuStartMs: 0,
    ...overrides,
  };
}

function runScenario(scenario: Scenario): void {
  configure(scenario.custom);
  totals.runs++;
  counts.error = counts.warn = counts.log = 0;
  lines.push(`run ${scenario.custom ? 1 : 0}`);
  let lastVcuHigh = 30;
  let thermalIndex = 0;
  // 0x200 at 20 Hz, 0x660 at 1 Hz, as on the bike.
  for (nowMs = 0; nowMs < scenario.durationMs; nowMs += 50) {
    if (nowMs >= scenario.vcuStartMs) {
      lastVcuHigh = 28 + Math.floor((nowMs / 1000) % 10);
      const low = lastVcuHigh - 3;
      const frame = Buffer.from([low & 0xff, 80, 98, lastVcuHigh & 0xff, 0x0f, 0xa0, 0xff, 0x38]);
      emit(0x200, frame);
    }
    if (scenario.thermalStartMs !== null && nowMs >= scenario.thermalStartMs && nowMs % 1000 === 0) {
      const length = scenario.thermalLength(thermalIndex);
      const trueHigh = scenario.trueHigh(thermalIndex);
      const full = Buffer.from([
        3,
        9,
        (trueHigh - 2) & 0xff,
        trueHigh & 0xff,
        (trueHigh - 4) & 0xff,
        scenario.clampGate(thermalIndex),
        Math.max(0, trueHigh - 35),
        (lastVcuHigh + scenario.echoOffset(thermalIndex)) & 0xff,
      ]);
      emit(0x660, full.subarray(0, length));
      thermalIndex++;
    }
  }
  lines.push(`logs ${counts.error} ${counts.warn} ${counts.log}`);
  totals.errors += counts.error;
  totals.warnings += counts.warn;
  totals.infos += counts.log;
}

function emit(id: number, frame: Buffer): void {
  const values = resolve(id, frame, decodeFrame(id, frame));
  totals.frames++;
  let line = `${nowMs} ${id.toString(16)} ${frame.toString("hex")}`;
  for (const { key, value } of values) {
    if (id === 0x200 && (key === "batt_temp_lo" || key === "batt_temp_hi")) totals.trueKeys++;
    line += ` ${key}=${String(value)}`;
  }
  lines.push(line);
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
