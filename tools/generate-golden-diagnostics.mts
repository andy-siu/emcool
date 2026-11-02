// Runs cool-eva's own diagnostic decoders on random input and writes what they returned, for
// host/tests/check_diagnostics.cpp to hold the C++ port to:
//   - freeze frames (src/diagnostics/freeze-frame.ts), shaped from Energica's shortlists and not
//   - the lifetime statistics (src/diagnostics/lifetime-stats.ts) from components 51 and 52
//   - the 0x18 stored-code list and the components a read would ask about (multiframe-codec.ts,
//     freeze-frame-list.ts)
//   - the hub's active fault list off 0x410 (src/can/hub-mirror.ts), every signal and log line
//
//   node tools/generate-golden-diagnostics.mts <cool-eva checkout> <output file>

import { registerHooks } from "node:module";
import { writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [coolEvaDirectory, outputPath] = process.argv.slice(2);
if (!coolEvaDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-diagnostics.mts <cool-eva checkout> <output file>");
  process.exit(2);
}
const srcRoot = pathToFileURL(resolve(coolEvaDirectory, "src")).href + "/";
let generation = 0;
registerHooks({
  resolve(specifier, context, nextResolve) {
    const result = nextResolve(specifier, context);
    const parent = context.parentURL;
    if (parent && result.url.startsWith(srcRoot) && !result.url.includes("?")) {
      const tag = new URL(parent).searchParams.get("gen");
      if (tag !== null) return { ...result, url: `${result.url}?gen=${tag}` };
    }
    return result;
  },
  load(url, context, nextLoad) {
    const result = nextLoad(url, context);
    if (url.startsWith(srcRoot + "can/signals.ts")) {
      const marker = "export function record(key: string, value: number, ts: number = Date.now()): void {";
      return { ...result, source: String(result.source).replace(marker, `${marker} (globalThis as any).__recorded(key, value);`) };
    }
    return result;
  },
});
const load = (path: string) => import(`${srcRoot}${path}?gen=${generation}`);

const lines: string[] = [];
const emit = (line: string) => void lines.push(line);
(globalThis as any).__recorded = (key: string, value: number) => emit(`HS ${key} ${value}`);
const quiet = console.error;
console.log = (...args: unknown[]) => emit(`HL I ${String(args[0])}`);
console.warn = (...args: unknown[]) => emit(`HL W ${String(args[0])}`);
console.error = (...args: unknown[]) => emit(`HL E ${String(args[0])}`);

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
const random = mulberry32(0x17);
const chance = (probability: number) => random() < probability;
const between = (low: number, high: number) => low + Math.floor(random() * (high - low + 1));
const pick = <T,>(items: readonly T[]): T => items[Math.floor(random() * items.length)];
const randomBytes = (count: number) => Array.from({ length: count }, () => between(0, 255));
const hex = (bytes: ArrayLike<number>) => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("") || "-";

generation = 1;
const freezeFrame = await load("diagnostics/freeze-frame.ts");
const faultInfokeys = await load("diagnostics/fault-infokeys.ts");
const infokeyTable = await load("diagnostics/infokey-table.ts");
const lifetimeStats = await load("diagnostics/lifetime-stats.ts");
const multiframe = await load("vcu/multiframe-codec.ts");
const list = await load("vcu/freeze-frame-list.ts");
const shortlists: { component: number; symptom: number; infokeys: number[] }[] = faultInfokeys.FAULT_INFOKEYS;

// A 0x17 reply for (component, symptom): the header, a value per shortlisted field, the cycles byte.
function freezeFramePayload(component: number, symptom: number, shape: () => number[] | null = () => null): number[] {
  const shortlist = faultInfokeys.infokeysFor(component, symptom);
  const fields: number[] = [];
  for (const id of shortlist?.infokeys ?? []) {
    const field = infokeyTable.lookupInfokey(id);
    const width = infokeyTable.infokeyWidth(field.datatype);
    fields.push(...(shape() ?? randomBytes(width)).slice(0, width));
    while (fields.length % 1 !== 0) fields.push(0);
  }
  const status = (symptom << 4) | pick([0x0d, 0x05, 0x09, 0x00, 0x04, between(0, 15)]);
  let payload = [0x57, between(0, 3), component >> 8, component & 0xff, status, ...fields];
  if (chance(0.85)) payload.push(between(0, 255));
  if (chance(0.08)) payload = payload.slice(0, between(1, payload.length));
  if (chance(0.05)) payload.push(...randomBytes(between(1, 3)));
  return payload;
}

let decoded = 0;
for (let index = 0; index < 5000; index++) {
  const known = chance(0.85);
  const entry = pick(shortlists);
  const component = known ? entry.component : between(0, 70);
  const symptom = known ? entry.symptom : between(0, 15);
  let payload: number[];
  const shape = random();
  if (shape < 0.75) payload = freezeFramePayload(component, symptom);
  else if (shape < 0.85) payload = [0x7f, pick([0x17, 0x18, between(0, 255)]), between(0, 255)].slice(0, between(1, 3));
  else if (shape < 0.9) payload = [0x57, 0];
  else payload = randomBytes(between(0, 10));
  const requested = chance(0.95) ? component : between(1, 63);
  const response = freezeFrame.decodeFreezeFrameResponse(Uint8Array.from(payload), requested);
  emit(`FF ${requested} ${hex(payload)} ${freezeFrame.isNoStoredRecordReply(Uint8Array.from(payload)) ? 1 : 0} ${JSON.stringify(response)}`);
  if (response.kind === "frame") for (const value of response.frame.values) emit(`FV ${JSON.stringify(freezeFrame.formatFreezeFrameValue(value))}`);
  emit("FE");
  decoded++;
}

// Lifetime: components 51 and 52, mostly frames, values sometimes outside their bands.
const lifetimeShortlists = (component: number) => shortlists.filter(entry => entry.component === component);
let summarised = 0;
for (let index = 0; index < 1500; index++) {
  const entries: { component: number; hex: string | null; reason: string | null }[] = [];
  for (const component of [51, 52]) {
    if (chance(0.08)) continue;
    if (chance(0.12)) {
      entries.push({ component, hex: chance(0.5) ? hex(randomBytes(between(0, 6))) : null, reason: pick(["no-response", "abandoned", "refused: 0x22 (conditionsNotCorrect) (NRC 0x22)"]) });
      continue;
    }
    const symptoms = lifetimeShortlists(component);
    const symptom = symptoms.length > 0 && chance(0.9) ? pick(symptoms).symptom : between(0, 15);
    const payload = freezeFramePayload(component, symptom, () => (chance(0.3) ? [pick([0x0f, 0x10, 0x00, 0xff]), between(0, 255), 0, 0] : null));
    entries.push({ component, hex: hex(payload), reason: null });
  }
  const responses = entries.map(entry => ({
    component: entry.component,
    response:
      entry.reason !== null
        ? { kind: "unrecognised", reason: entry.reason, rawHex: entry.hex === null ? "" : entry.hex }
        : freezeFrame.decodeFreezeFrameResponse(Uint8Array.from(Buffer.from(entry.hex!, "hex")), entry.component),
  }));
  const readAt = 1_790_000_000_000 + index;
  emit(`LT ${readAt} ${JSON.stringify(entries)} ${JSON.stringify(lifetimeStats.summariseLifetimeStatistics(readAt, responses))}`);
  summarised++;
}

// The 0x18 list.
for (let index = 0; index < 2000; index++) {
  const declared = between(0, 12);
  const body = [chance(0.9) ? declared : between(0, 255)];
  for (let record = 0; record < declared + (chance(0.2) ? between(-2, 2) : 0); record++) {
    const code = chance(0.8) ? between(1, 63) : chance(0.5) ? 0 : between(64, 0xffff);
    body.push(code >> 8, code & 0xff, code === 0 && chance(0.7) ? 0 : between(0, 255));
  }
  if (chance(0.1)) body.push(...randomBytes(between(1, 2)));
  const used = chance(0.03) ? [] : body;
  const stored = multiframe.decodeStoredDtcList(Uint8Array.from(used));
  const selection = list.selectComponentsToRead(stored);
  emit(`SL ${hex(used)} ${JSON.stringify(stored)} ${JSON.stringify(selection)} ${JSON.stringify(list.describeListDamage(selection.report))}`);
}

// The hub's fault list, through a fresh instance each run.
let hubFrames = 0;
const tableKeys: { component: number; symptom: number }[] = (await load("diagnostics/dtc-table.ts")).DTC_TABLE;
for (let run = 0; run < 60; run++) {
  generation = 100 + run;
  const hub = await load("can/hub-mirror.ts");
  emit("HI");
  const pool = Array.from({ length: between(1, 12) }, () => {
    if (chance(0.7)) {
      const entry = pick(tableKeys);
      return entry.component | (entry.symptom << 16);
    }
    return chance(0.5) ? between(1, 0xfffff) : pick([0x0514, 0x1046, 0xc111]);
  });
  const many = run % 15 === 7;
  for (let report = 0; report < between(3, 40); report++) {
    const codes = many ? Array.from({ length: 40 }, () => between(1, 0xfffff)) : pool.filter(() => chance(0.6));
    const pages = Math.max(1, Math.ceil(codes.length / 2));
    for (let page = 0; page < pages; page++) {
      const sub = pages === 1 ? pick([0xff, 0xff, 0x00]) : page === 0 ? 0x00 : page === pages - 1 ? 0xfe : page;
      const frame = [25, sub];
      for (const code of [codes[page * 2] ?? 0, codes[page * 2 + 1] ?? 0]) {
        const flags = chance(0.2) ? between(1, 15) : 0;
        frame.push(code & 0xff, (code >> 8) & 0xff, ((code >> 16) & 0x0f) | (flags << 4));
      }
      if (chance(0.03)) frame.length = between(2, 7);
      emit(`HF ${hex(frame)}`);
      hub.handleHubMirrorFrame(Buffer.from(frame));
      hubFrames++;
    }
    if (chance(0.3)) {
      const side = [31, ...(chance(0.7) ? [1, 2, 3, 4, 5, 6, 7] : randomBytes(7))];
      emit(`HF ${hex(side)}`);
      hub.handleHubMirrorFrame(Buffer.from(side));
      hubFrames++;
    }
  }
  emit("HE");
}

await writeFile(outputPath, lines.join("\n") + "\n");
quiet(`golden-diagnostics: ${decoded} freeze-frame replies, ${summarised} lifetime summaries, 2000 code lists, ${hubFrames} hub frames`);
