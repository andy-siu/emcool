// Runs cool-eva's own VCU code on random inputs and writes what it returned, for
// host/tests/check_vcu.cpp to hold the C++ port to. The pure parts: extended-addressing ISO-TP, the
// parameter and multi-frame codecs, record interpretation, the service gate, read outcomes as rows,
// the TABLE_TYPE report and re-naming a snapshot from it, snapshot diffs, and the table catalogue.
//
//   node tools/generate-golden-vcu.mts <cool-eva checkout> <output file>
//
// Each line is `<kind> <fields…>`; JSON fields are JSON.stringify's, with byte arrays as hex.

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [coolEvaDirectory, outputPath] = process.argv.slice(2);
if (!coolEvaDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-vcu.mts <cool-eva checkout> <output file>");
  process.exit(2);
}
const load = (path: string) => import(pathToFileURL(join(coolEvaDirectory, path)).href);
const quiet = { log: console.log, warn: console.warn, error: console.error };
console.log = console.warn = console.error = () => {};
const report = (line: string) => quiet.error(line);

const extended = await load("src/diagnostics/extended-iso-tp.ts");
const paramCodec = await load("src/vcu/param-codec.ts");
const multiframe = await load("src/vcu/multiframe-codec.ts");
const gate = await load("src/vcu/service-gate.ts");
const snapshot = await load("src/vcu/snapshot.ts");
const paramTable = await load("src/vcu/param-table.ts");
const catalogue = await load("src/vcu/table-catalog.ts");
const firmware = await load("src/vcu/a8-firmware-rows.ts");

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
const random = mulberry32(0x7c0);
const chance = (probability: number) => random() < probability;
const between = (low: number, high: number) => low + Math.floor(random() * (high - low + 1));
const pick = <T,>(items: readonly T[]): T => items[Math.floor(random() * items.length)];
const randomBytes = (count: number) => Array.from({ length: count }, () => between(0, 255));
const hex = (bytes: ArrayLike<number>) => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("") || "-";
const json = (value: unknown) =>
  JSON.stringify(value, (_, inner) => (inner instanceof Uint8Array ? hex(inner) : inner));

const lines: string[] = [];
const emit = (line: string) => lines.push(line);
const counts: Record<string, number> = {};
const count = (kind: string) => (counts[kind] = (counts[kind] ?? 0) + 1);

// --- Extended-addressing ISO-TP ---------------------------------------------------------------
for (let sequence = 0; sequence < 2500; sequence++) {
  const cap = pick([32, 64, 256]);
  const reassembler = new extended.ExtendedIsoTpReassembler(cap);
  emit(`XI ${cap}`);
  let nextSequence = 0;
  for (let push = between(1, 30); push > 0; push--) {
    let frame: number[];
    const shape = random();
    const address = chance(0.92) ? 0xf1 : pick([0xa8, 0xa9, 0x00, between(0, 255)]);
    if (shape < 0.15) {
      const total = chance(0.15) ? between(cap - 3, cap + 40) : between(0, Math.min(cap, 80));
      frame = [address, 0x10 | ((total >> 8) & 0x0f), total & 0xff, ...randomBytes(5)];
      if (chance(0.05)) frame = frame.slice(0, between(2, 7));
      nextSequence = 0;
    } else if (shape < 0.7) {
      frame = [address, 0x20 | (chance(0.93) ? nextSequence : between(0, 15)), ...randomBytes(chance(0.85) ? 6 : between(0, 6))];
      nextSequence = (nextSequence + 1) & 0x0f;
    } else if (shape < 0.85) {
      const length = chance(0.8) ? between(1, 6) : between(0, 15);
      frame = [address, length, ...randomBytes(chance(0.85) ? 6 : between(0, 6))];
    } else {
      frame = randomBytes(between(0, 8));
    }
    const result = extended.ExtendedIsoTpReassembler.prototype.push.call(reassembler, Uint8Array.from(frame));
    emit(
      `XP ${hex(frame)} ${result.status} ${result.status === "flow-control-required" ? result.totalLength : 0} ` +
        `${result.status === "complete" ? hex(result.payload) : "-"} ${result.reason ?? "-"}`
    );
    count("extended ISO-TP pushes");
  }
}

// --- The parameter codec ----------------------------------------------------------------------
for (let index = 0; index < 3000; index++) {
  const frame = chance(0.7) ? [0xf1, ...randomBytes(between(1, 7))] : randomBytes(between(0, 8));
  if (frame.length > 1 && chance(0.6)) frame[1] = (pick([0, 0, 1, 2, 3, 4]) << 4) | between(0, 7);
  emit(`PF ${hex(frame)} ${json(paramCodec.parseResponseFrame(Uint8Array.from(frame)))}`);
  count("response frames parsed");
}
for (let index = 0; index < 4000; index++) {
  const identifier = between(0, 0xffff);
  let payload: number[];
  const shape = random();
  if (shape < 0.5) {
    const echoed = chance(0.85) ? identifier : between(0, 0xffff);
    payload = [chance(0.95) ? 0x62 : between(0, 255), echoed >> 8, echoed & 0xff, ...randomBytes(between(0, 6))];
    if (chance(0.05)) payload = payload.slice(0, between(0, 3));
  } else if (shape < 0.8) {
    payload = [0x7f, ...randomBytes(between(0, 3))];
  } else {
    payload = randomBytes(between(0, 5));
  }
  emit(`PR ${hex(payload)} ${identifier} ${json(paramCodec.decodeParameterReply(Uint8Array.from(payload), identifier))}`);
  count("parameter replies decoded");
}
const storageTypes = ["BYTE", "WORD", "BOOL", "DWORD"];
for (let index = 0; index < 4000; index++) {
  const record = randomBytes(chance(0.8) ? pick([1, 2, 4]) : between(0, 12));
  const encoding = chance(0.15)
    ? null
    : {
        type: pick(storageTypes),
        signed: chance(0.15) ? null : chance(0.5),
        ...(chance(0.3) ? { describedBy: "A8's firmware table" } : {}),
      };
  const interpreted = paramCodec.interpretRecord(Uint8Array.from(record), encoding);
  const mismatch = encoding && interpreted.widthMismatch ? paramCodec.describeWidthMismatch(record.length, encoding) : null;
  emit(`IR ${hex(record)} ${json(encoding)} ${json(interpreted)} ${json(mismatch)}`);
  count("records interpreted");
}

// --- Multi-frame framing ----------------------------------------------------------------------
for (let index = 0; index < 600; index++) {
  const micro = pick(["A8", "A9"]);
  const payload = randomBytes(chance(0.5) ? between(1, 6) : between(7, 60));
  emit(`SEG ${micro} ${hex(payload)} ${multiframe.segmentRequestPayload(micro, Uint8Array.from(payload)).map(hex).join(",")}`);
  count("requests segmented");
}
for (let index = 0; index < 2000; index++) {
  const frame = chance(0.7) ? [0xf1, 0x30 | between(0, 4), ...randomBytes(between(0, 6))] : randomBytes(between(0, 8));
  emit(`FC ${hex(frame)} ${json(multiframe.parseFlowControlFrame(Uint8Array.from(frame)))}`);
  count("flow controls parsed");
}
for (let index = 0; index < 2000; index++) {
  const expected = pick([0x57, 0x58, 0x75, 0x76, 0x77]);
  const payload =
    random() < 0.5
      ? [chance(0.9) ? expected : between(0, 255), ...randomBytes(between(0, 8))]
      : [0x7f, chance(0.8) ? expected - 0x40 : between(0, 255), ...randomBytes(between(0, 2))];
  emit(`MR ${hex(payload)} ${expected} ${json(multiframe.decodeMultiFrameReply(Uint8Array.from(payload), expected))}`);
  count("multi-frame replies decoded");
}

// --- The service gate -------------------------------------------------------------------------
const gateKeys: string[] = gate.serviceGateSignalKeys();
for (let index = 0; index < 6000; index++) {
  const readings: Record<string, { value: number | null; ageMs: number | null }> = {};
  const charging = chance(0.3);
  for (const key of gateKeys) {
    if (chance(0.1)) {
      readings[key] = { value: null, ageMs: null };
      continue;
    }
    let value: number;
    if (key === "charge_manager_state") value = pick([0x02, 0x23, 0, 0x10, between(0, 255)]);
    else if (key === "charge_manager_status") value = pick([0x08, 0x0c, 0, 0x04, between(0, 255)]);
    else if (["dc_v", "dc_a", "mains_v", "mains_a"].includes(key)) value = charging ? between(0, 4000) / 10 : 0;
    else value = chance(0.85) ? 0 : pick([1, 0.5, 2.25, between(1, 90), -0.25, 0.05]);
    const age = chance(0.8) ? between(0, 900) : pick([between(1000, 2500), between(5000, 20000), 1000, 10000, 5000, 2000]);
    readings[key] = { value, ageMs: chance(0.1) ? null : age + pick([0, 0, 0.5, 0.25]) };
  }
  const flat = gateKeys
    .map(key => `${key}=${readings[key].value ?? "null"}@${readings[key].ageMs ?? "null"}`)
    .join(";");
  emit(`SG ${flat} ${JSON.stringify(gate.evaluateServiceGate(readings))}`);
  emit(`SR ${flat} ${JSON.stringify(gate.restartMotionBlockers(readings))}`);
  count("gate verdicts");
}

// --- The catalogue ----------------------------------------------------------------------------
const known: number[] = catalogue.KNOWN_TABLE_TYPES;
for (const raw of [...known, 0, 1, 0xffff, 65536, 4097, -1, -4096, 123456, ...Array.from({ length: 300 }, () => between(-70000, 70000))]) {
  emit(`DT ${raw} ${JSON.stringify(catalogue.describeTableType(raw))} ${JSON.stringify(catalogue.contentTwinsOf(raw))}`);
  count("table types described");
}

// --- Rows, the TABLE_TYPE report, re-naming and diffs -------------------------------------------
const statuses = ["read", "refused", "no-response", "no-session", "stalled", "abandoned", "unrecognised", "not-sent"];
const interesting = [276, 277, 1, 2, 70, 94, 150, 223, 278, 279, 613, 622, 627, 1000, 1006, 1007, 2000, 4095];
function randomOutcome(index: number, micro: string) {
  const status = chance(0.7) ? "read" : pick(statuses);
  const identifier = (1 << 12) | index;
  const base = { micro, index, identifier, status, flowControlLatency: null };
  switch (status) {
    case "read": {
      const table = paramTable.parameterAtIndex(index);
      const width = chance(0.85) ? (table ? paramTable.recordLengthFor(table.type) : pick([1, 2, 4])) : between(1, 6);
      let record = randomBytes(width);
      if ((index === 276 || index === 277) && chance(0.8)) {
        const type = chance(0.85) ? pick(known) : between(0, 0xffff);
        record = width === 2 ? [type >> 8, type & 0xff] : record;
      }
      return { ...base, record: Uint8Array.from(record) };
    }
    case "refused": {
      const code = pick([0x11, 0x12, 0x22, 0x31, 0x33, 0x78, 0x99]);
      return { ...base, negativeResponseCode: code, description: (paramCodec as any).describeNegativeResponseCode?.(code) ?? `0x${code.toString(16)}` };
    }
    case "no-response":
      return base;
    default:
      return { ...base, reason: pick(["A9 did not answer 10 81", "a First Frame arrived and the rest of the reply never did", "client stopped", "reply echoed identifier 0x1001, not 0x1002"]) };
  }
}
const obdDtc = await load("src/diagnostics/obd-dtc.ts");
for (let round = 0; round < 1500; round++) {
  const tableType = chance(0.6) ? paramTable.DEFAULT_TABLE_TYPE : pick(known);
  paramTable.selectParameterTable(tableType);
  const rows = [];
  const indices = [...new Set([276, 277, ...Array.from({ length: between(0, 8) }, () => (chance(0.6) ? pick(interesting) : between(1, 320)))])];
  for (const index of indices) {
    if ((index === 276 || index === 277) && chance(0.1)) continue;
    const micro = paramTable.parameterAtIndex(index)?.micro ?? (chance(0.5) ? "A8" : "A9");
    const outcome = randomOutcome(index, micro);
    if (outcome.status === "refused") (outcome as any).description = obdDtc.describeNegativeResponseCode((outcome as any).negativeResponseCode);
    emit(`OUT ${tableType} ${json(outcome)}`);
    const row = snapshot.toParameterRow(outcome);
    emit(`TPR ${JSON.stringify(row)}`);
    if (chance(0.15)) Object.assign(row, { rawHex: pick(["zz 01", "1 2 3", "", " 0A  ff "]) });
    if (chance(0.1)) row.note = null;
    emit(`ROW ${json(row)} ${JSON.stringify(snapshot.describeRow(row))}`);
    rows.push(row);
    count("rows");
  }
  rows.sort((left, right) => left.index - right.index);
  const swept = { readAt: 1780000000000 + round, complete: chance(0.5), micros: ["A9", "A8"], rows };
  const tableReport = snapshot.reportTableType(swept);
  emit(`REP ${tableType} ${JSON.stringify(tableReport)}`);
  const retabled = snapshot.retableSnapshot(swept, tableReport);
  for (const row of retabled.rows) emit(`RET ${JSON.stringify(row)}`);
  emit("RETEND");
  count("reports");
  // A diff against a perturbed copy.
  const before = retabled.rows.filter(() => chance(0.85)).map((row: any) => ({ ...row }));
  for (const row of before) {
    if (chance(0.2) && row.rawHex) row.rawHex = hex(randomBytes(2)).toUpperCase();
    if (chance(0.1)) row.status = pick(statuses);
  }
  const extra = chance(0.3) && retabled.rows.length > 0 ? [{ ...retabled.rows[0], index: 9000 + round }] : [];
  const previous = { ...swept, rows: [...before, ...extra].sort((left, right) => left.index - right.index) };
  for (const row of previous.rows) emit(`DPREV ${JSON.stringify(row)}`);
  for (const change of snapshot.diffSnapshots(previous, retabled)) emit(`DIFF ${JSON.stringify(snapshot.describeChange(change))}`);
  emit("DIFFEND");
}
paramTable.selectParameterTable(paramTable.DEFAULT_TABLE_TYPE);
emit(`USE ${paramTable.DEFAULT_TABLE_TYPE}`);

// --- Table selection --------------------------------------------------------------------------
for (let round = 0; round < 200; round++) {
  const tableType = chance(0.8) ? pick(known) : between(0, 0xffff);
  emit(`SEL ${tableType} ${JSON.stringify(paramTable.selectParameterTable(tableType))}`);
  count("table selections");
}

// --- The firmware rows ------------------------------------------------------------------------
for (const row of firmware.a8FirmwareRows()) emit(`FW ${row.index} ${JSON.stringify(firmware.describeFirmwareRow(row))}`);

await writeFile(outputPath, lines.join("\n") + "\n");
report(`golden-vcu: ${Object.entries(counts).map(([kind, total]) => `${total} ${kind}`).join(", ")}`);
