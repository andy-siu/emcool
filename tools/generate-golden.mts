// Runs a deterministic corpus of CAN frames through the reference's own decodeFrame() and writes
// each frame with the values it produced, for host/tests/check_decoders.cpp to replay through
// the C++ port and compare value-for-value. The 16 GB capture archive the TypeScript was
// verified on is not on this machine, so the corpus is synthetic: every 11-bit id at every
// length, biased-random payloads for every decoded id (gated bytes drawn from the values
// their gates test for), edge patterns, and GPS sub-frame sequences. It fails if any key the
// decoders can emit never appears, because a key the corpus never reaches is a key the diff
// never checks.
//
//   node tools/generate-golden.mts <reference checkout> <output file>
//
// Line format: `<id hex> <payload hex, or - when empty>[ <key>=<value>]...`, with values
// written by String(), the shortest form that parses back to the identical double.

import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

interface DecodedValue {
  key: string;
  value: number;
}

type ByteChoice = readonly number[] | null;

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden.mts <reference checkout> <output file>");
  process.exit(2);
}

// The corpus deliberately feeds the decoders out-of-range attitude and half-complete GPS fixes,
// so the reference warns. Count instead of printing: the count is written beside the corpus, and
// tools/check.sh requires the C++ port to warn exactly as often.
let decoderWarnings = 0;
console.warn = () => {
  decoderWarnings++;
};

const decodeModule = await import(pathToFileURL(join(referenceDirectory, "src/can/decode.ts")).href);
const decodeFrame = decodeModule.decodeFrame as (id: number, data: Buffer) => DecodedValue[];
const streamIds = decodeModule.STREAM_IDS as number[];
const registryModule = await import(pathToFileURL(join(referenceDirectory, "src/can/registry.ts")).href);
const registeredKeys = new Set((registryModule.SIGNALS as { key: string }[]).map(signal => signal.key));

const random = mulberry32(0x0e3c001);
const lines: string[] = [];
const emittedKeys = new Set<string>();

const range = (low: number, highInclusive: number): number[] =>
  Array.from({ length: highInclusive - low + 1 }, (_, index) => low + index);
const signedTemperatureBytes = [...range(0, 105), ...range(0xd3, 0xff)];
const cellVoltageHighBytes = range(0x03, 0x13);
const attitudeHighBytes = [...range(0x00, 0x07), ...range(0xf8, 0xff)];

// Per-id byte candidates for decoders whose gates uniform bytes would almost never pass.
const BYTE_CHOICES: Record<number, ByteChoice[]> = {
  0x121: [
    [0x18, 0x1a, 0x2c, 0x98, 0x9a],
    [0xff],
    [0, 1, 2, 10, 50, 100, 101, 150, 255],
    [0, 1],
    [0, 1, 10, 60, 100, 200, 255],
    [0],
    [0],
    [0],
  ],
  0x610: [null, null, null, null, [0xf1], null, null, null],
  0x615: [[0, 1, 0x40, 0x80], [0x01], null, null, [0], [0], [0], [0]],
  0x620: [null, null, null, [0, 1, 7], [0], [0], [0], [0]],
  0x625: [null, [0x01], null, [0xff], null, null, null, null],
  0x10b: [[0x00, 0xe8], [0x00, 0xfd, 0xfe, 0xff], null, null, [0x00, 0xe8], [0x00, 0xfd, 0xfe, 0xff], null, null],
  0x102: [null, null, null, null, null, attitudeHighBytes, null, attitudeHighBytes],
  0x410: [[26, 3], [0x00, 0x01, 0xfe, 0xff], null, null, null, null, null, null],
  0x660: [null, null, signedTemperatureBytes, signedTemperatureBytes, signedTemperatureBytes, [0, 255], null, signedTemperatureBytes],
  0x662: [range(0, 12), cellVoltageHighBytes, null, cellVoltageHighBytes, null, cellVoltageHighBytes, null, null],
  0x663: [range(0, 12), cellVoltageHighBytes, null, cellVoltageHighBytes, null, cellVoltageHighBytes, null, null],
  0x664: [range(0, 12), cellVoltageHighBytes, null, cellVoltageHighBytes, null, signedTemperatureBytes, signedTemperatureBytes, signedTemperatureBytes],
};

const decodedIds = [...new Set([...streamIds, 0x121, 0x410])].sort((left, right) => left - right);

// 1. Every 11-bit id at every length: unknown ids must decode to nothing in both.
for (let id = 0; id <= 0x7ff; id++) {
  for (let length = 0; length <= 8; length++) {
    for (let sample = 0; sample < 2; sample++) {
      emit(id, uniformBytes(length));
    }
  }
}

// 2. Every decoded id: short frames exercise the length guards, full frames the fields.
for (const id of decodedIds) {
  for (let length = 0; length < 8; length++) {
    for (let sample = 0; sample < 60; sample++) {
      emit(id, biasedBytes(id, length));
    }
  }
  for (let sample = 0; sample < 3000; sample++) {
    emit(id, biasedBytes(id, 8));
  }
  for (const payload of edgePayloads()) {
    emit(id, payload);
  }
}

// 3. GPS: sub-frames in random order with random omissions, so a fix sometimes arrives with
// one coordinate half missing (the suppression path) and the UTC fields roll over months.
for (let sample = 0; sample < 20000; sample++) {
  const subFrame = [0x00, 0x01, 0xfe][Math.floor(random() * 3)];
  const payload = uniformBytes(8);
  payload[0] = 26;
  payload[1] = subFrame;
  emit(0x410, payload);
}

const decoderKeys = await keysNamedInDecoderSources();
const missing = [...decoderKeys].filter(key => !emittedKeys.has(key)).sort();
const unregistered = [...emittedKeys].filter(key => !registeredKeys.has(key)).sort();

await writeFile(outputPath, lines.join("\n") + "\n");
await writeFile(`${outputPath}.warnings`, `${decoderWarnings}\n`);
console.log(
  `golden: ${lines.length} frames, ${emittedKeys.size} distinct keys, ${decoderWarnings} decoder warnings → ${outputPath}`
);
if (unregistered.length > 0) {
  console.error(`golden: decoders emit keys the reference's registry lacks: ${unregistered.join(", ")}`);
  process.exit(1);
}
if (missing.length > 0) {
  console.error(`golden: the corpus never produced ${missing.length} decoder keys: ${missing.join(", ")}`);
  process.exit(1);
}

function emit(id: number, payload: Buffer): void {
  const values = decodeFrame(id, payload);
  let line = `${id.toString(16).padStart(3, "0")} ${payload.length > 0 ? payload.toString("hex") : "-"}`;
  for (const { key, value } of values) {
    emittedKeys.add(key);
    line += ` ${key}=${String(value)}`;
  }
  lines.push(line);
}

function uniformBytes(length: number): Buffer {
  const bytes = Buffer.alloc(length);
  for (let index = 0; index < length; index++) {
    bytes[index] = Math.floor(random() * 256);
  }
  return bytes;
}

function biasedBytes(id: number, length: number): Buffer {
  const bytes = uniformBytes(length);
  const choices = BYTE_CHOICES[id];
  if (!choices) {
    return bytes;
  }
  for (let index = 0; index < length; index++) {
    const candidates = choices[index];
    if (candidates && random() < 0.85) {
      bytes[index] = candidates[Math.floor(random() * candidates.length)];
    }
  }
  return bytes;
}

function edgePayloads(): Buffer[] {
  const payloads = [0x00, 0xff, 0x80, 0x7f].map(fill => Buffer.alloc(8, fill));
  for (let bitIndex = 0; bitIndex < 64; bitIndex++) {
    const payload = Buffer.alloc(8);
    payload[bitIndex >> 3] = 1 << (bitIndex & 7);
    payloads.push(payload);
  }
  return payloads;
}

// Every literal `key: "…"` in the modules decodeFrame reaches, plus the per-LMU keys that
// decode-bms.ts builds at runtime (taken from the registry, which defines them all).
async function keysNamedInDecoderSources(): Promise<Set<string>> {
  const modules = [
    "abs", "attitude", "charge-manager", "charge-setpoint", "charge-soc-limit", "cluster-range",
    "consumption", "decode", "decode-bms", "drive", "hub-output", "psu", "vcu-digitals",
    "vcu-flags", "vehicle-status",
  ].map(name => join(referenceDirectory, `src/can/${name}.ts`));
  modules.push(join(referenceDirectory, "src/gps/decode.ts"));
  const keys = new Set<string>();
  for (const path of modules) {
    const source = await readFile(path, "utf-8");
    for (const match of source.matchAll(/key: "([a-z0-9_]+)"/g)) {
      keys.add(match[1]);
    }
    for (const match of source.matchAll(/\[0x[0-9a-f]+, "(bms_state_[a-z_]+)"\]/g)) {
      keys.add(match[1]);
    }
  }
  for (const key of registeredKeys) {
    if (/^lmu\d+_(cell\d+_mv|bat1_c|pcb[12]_c)$/.test(key)) {
      keys.add(key);
    }
  }
  return keys;
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
