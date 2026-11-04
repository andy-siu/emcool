// Runs the reference's own VCU WRITE code on random/scripted input and writes what it returned, for
// host/tests/check_write.cpp to hold the C++ port to. The pure parts of stage 4: the security-key
// algorithm, the write/routine/security/clear-DTC reply decoders, the service-stamp interpreter,
// the clock verdict, the RTC-sync frame both directions, the allowlist and planning, the table
// gate, and the /vcu-write request/status parser.
//
//   node tools/generate-golden-write.mts <reference checkout> <output file>
//
// Each line is `<kind> <fields…>`; JSON fields are JSON.stringify's, byte arrays as hex.

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  console.error("usage: node tools/generate-golden-write.mts <reference checkout> <output file>");
  process.exit(2);
}
const load = (path: string) => import(pathToFileURL(join(referenceDirectory, path)).href);
console.log = console.warn = console.error = () => {};

const writeCodec = await load("src/vcu/write-codec.ts");
const writeTargets = await load("src/vcu/write-targets.ts");
const tableGate = await load("src/vcu/table-gate.ts");
const serviceActions = await load("src/vcu/service-actions.ts");
const paramTable = await load("src/vcu/param-table.ts");
const catalogue = await load("src/vcu/table-catalog.ts");
const httpWrite = await load("src/http/vcu-write.ts");
const chargeCommand = await load("src/can/charge-command.ts");
const acknowledge = await load("src/charge/acknowledge.ts");

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
const random = mulberry32(0x2e31);
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

// --- 1. The security-key algorithm ------------------------------------------------------------
for (let at = 0; at < 4000; at++) {
  const seed = at < 6 ? [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0xdeadbeef][at] : between(0, 0xffffffff);
  emit(`KEY ${seed >>> 0} ${writeCodec.securityKeyForSeed(seed) >>> 0}`);
  count("KEY");
}

// --- 2. The reply decoders --------------------------------------------------------------------
// SecurityAccess (0x67), parameter write (0x6E), routine (0x71) and clear-DTC (0x44) replies, from
// random payloads that mostly look like real ones.
function noisyReply(service: number): number[] {
  const shape = random();
  if (shape < 0.25) return [0x7f, service, pick([0x33, 0x35, 0x22, 0x31, between(0, 255)]), ...randomBytes(between(0, 3))];
  if (shape < 0.45) return randomBytes(between(0, 6));
  return [service + 0x40, ...randomBytes(between(0, 6))];
}
for (let at = 0; at < 3000; at++) {
  const payload = noisyReply(0x27);
  emit(`SA ${hex(payload)} ${json(writeCodec.decodeSecurityAccessReply(Uint8Array.from(payload)))}`);
  count("SA");
}
for (let at = 0; at < 3000; at++) {
  const identifier = between(0x1000, 0x1fff);
  const shape = random();
  const payload =
    shape < 0.5 ? [0x6e, identifier >> 8, identifier & 0xff, ...randomBytes(between(0, 3))] : noisyReply(0x2e);
  emit(`PW ${identifier} ${hex(payload)} ${json(writeCodec.decodeParameterWriteReply(Uint8Array.from(payload), identifier))}`);
  count("PW");
}
for (let at = 0; at < 2000; at++) {
  const payload = random() < 0.5 ? [0x71, pick([0xfc, between(0, 255)]), ...randomBytes(between(0, 3))] : noisyReply(0x31);
  emit(`RR ${hex(payload)} ${json(writeCodec.decodeRoutineReply(Uint8Array.from(payload), "set-service-point"))}`);
  count("RR");
}
for (let at = 0; at < 2000; at++) {
  const shape = random();
  const payload =
    shape < 0.4 ? [0x01, 0x44, ...randomBytes(between(0, 2))]
    : shape < 0.7 ? [0x03, 0x7f, pick([0x04, 0x00, between(0, 255)]), between(0, 255)]
                  : randomBytes(between(0, 6));
  emit(`CD ${hex(payload)} ${json(serviceActions.decodeClearDtcsReply(Uint8Array.from(payload)))}`);
  count("CD");
  // isClearDtcsReply is a separate predicate on the same bytes.
  emit(`CDR ${hex(payload)} ${serviceActions.isClearDtcsReply(Buffer.from(payload)) ? 1 : 0}`);
  count("CDR");
}

// --- 3. The service stamp ---------------------------------------------------------------------
for (let at = 0; at < 3000; at++) {
  const words = {
    dateLow: between(0, 0xffff),
    dateHigh: at < 4 ? [0, 0, 0xffff, between(0, 0xffff)][at] : between(0, 0xffff),
    odometerLow: between(0, 0xffff),
    odometerHigh: between(0, 0xffff),
  };
  if (at === 2) words.dateLow = 0xffff;
  const nowMs = 1790000000000 + between(-5, 5) * 86400000 * 365;
  emit(`ST ${words.dateLow} ${words.dateHigh} ${words.odometerLow} ${words.odometerHigh} ${nowMs} ${json(serviceActions.interpretServiceStamp(words, nowMs))}`);
  count("ST");
}

// --- 4. The Pi/bike clock verdict -------------------------------------------------------------
// ⚠️ Every field kept faithfully round-trippable: a NaN would JSON.stringify to `null` and the C++
// side would read it as a genuine null, a different input. So the clock is either a finite epoch or
// the string "nan" (emitted as a sentinel), never a bare NaN; and gpsEpochSeconds is derived only
// from a finite clock, so it is a real number or null, nothing that prints as null but isn't.
for (let at = 0; at < 3000; at++) {
  const floor = Date.UTC(2026, 7, 16);
  const shape = random();
  const systemEpochMs =
    shape < 0.8 ? floor + between(0, 10 * 365) * 86400000 + between(-120, 120) * 1000
    : shape < 0.9 ? NaN
    : pick([Date.UTC(2020, 0, 1), Date.UTC(2050, 0, 1)]);
  const clockFinite = Number.isFinite(systemEpochMs);
  const gpsEpochSeconds =
    !clockFinite ? (chance(0.5) ? null : between(0, 2000000000))
    : chance(0.85) ? Math.round(systemEpochMs / 1000) + between(-120, 120)
    : chance(0.5) ? null
                  : between(0, 2000000000);
  const gpsAgeMs = gpsEpochSeconds === null ? (chance(0.5) ? null : between(0, 300000)) : between(0, 300000);
  const evidence = { systemEpochMs, gpsEpochSeconds, gpsAgeMs };
  // Emit systemEpochMs as "nan" when it is NaN, since JSON would lose the distinction from null.
  const evidenceText = `${Number.isFinite(systemEpochMs) ? systemEpochMs : "nan"} ${gpsEpochSeconds === null ? "null" : gpsEpochSeconds} ${gpsAgeMs === null ? "null" : gpsAgeMs}`;
  emit(`CK ${evidenceText} ${json(serviceActions.checkPiClock(evidence))}`);
  count("CK");
}

// --- 5. The RTC-sync frame, both directions ---------------------------------------------------
for (let at = 0; at < 3000; at++) {
  const when = new Date(Date.UTC(between(2000, 2060), between(0, 11), between(1, 28), between(0, 23), between(0, 59), between(0, 59)));
  let frame: number[] | null = null;
  try {
    frame = Array.from(serviceActions.buildRtcSyncFrame(when) as Uint8Array);
  } catch {
    frame = null;
  }
  emit(`RTC ${when.getTime()} ${frame === null ? "-" : hex(frame)} ${frame === null ? "null" : json(serviceActions.decodeRtcSyncFrame(Uint8Array.from(frame)))}`);
  count("RTC");
}

// --- 6. The allowlist, planning, and the table gate -------------------------------------------
// The default table's writable list, with its controls — the dropdown the page builds.
const targets = writeTargets.writeTargets();
emit(`TARGETS ${targets.length}`);
count("TARGETS");
for (const target of targets) {
  emit(`TGT ${json({ name: target.name, index: target.index, control: target.control, minLabel: target.unit(target.control.kind === "number" ? target.control.min : 0), sample: target.unit(target.control.kind === "number" ? target.control.max : 4660) })}`);
  count("TGT");
}

// planWrite / planBitWrite over curated and generated names, in and out of range.
const curatedNames = writeTargets.CURATED_WRITE_TARGETS.map((t: { name: string }) => t.name);
const allNames = targets.map((t: { name: string }) => t.name);
for (let at = 0; at < 4000; at++) {
  const name = chance(0.6) ? pick(curatedNames) : chance(0.7) ? pick(allNames) : pick(["NOPE", "", "VSM_DUMMY_WORD8", pick(allNames).toLowerCase()]);
  const value = between(-40000, 70000);
  const previous = between(-40000, 70000);
  emit(`PLAN ${json({ name, value, previous })} ${json(writeTargets.planWrite(name, value, previous))}`);
  count("PLAN");
}
for (let at = 0; at < 1500; at++) {
  const name = chance(0.7) ? "VSM_CONFIG_1" : pick(allNames);
  const bit = pick(["heated-handlebars", "nope", "fast-charge"]);
  const on = chance(0.5);
  const current = between(0, 0x1ffff);
  emit(`BIT ${json({ name, bit, on, current })} ${json(writeTargets.planBitWrite(name, bit, on, current))}`);
  count("BIT");
}

// The table gate across the carried tables, split and unread.
const known: number[] = catalogue.KNOWN_TABLE_TYPES;
const snapshot = await load("src/vcu/snapshot.ts");
function tableReport(a9: number | null, a8: number | null, unusable: number[]): unknown {
  const verdicts: { index: number; micro: string; value: number }[] = [];
  if (a9 !== null) verdicts.push({ index: 276, micro: "A9", value: a9 });
  if (a8 !== null) verdicts.push({ index: 277, micro: "A8", value: a8 });
  return { verdicts, unusable: unusable ?? [] };
}
for (let at = 0; at < 2000; at++) {
  const shape = random();
  let report: unknown;
  if (shape < 0.4) {
    const t = pick(known);
    report = tableReport(t, t, []);
  } else if (shape < 0.55) {
    report = tableReport(pick(known), pick(known), []);
  } else if (shape < 0.7) {
    report = tableReport(between(0, 0xffff), null, []);
  } else if (shape < 0.82) {
    report = tableReport(null, null, chance(0.5) ? [276] : []);
  } else {
    report = chance(0.5) ? null : tableReport(pick(known), between(0, 0xffff), []);
  }
  // selectParameterTable may have moved the active table in a previous iteration; reset to default
  // so the gate's recheck is against a known table, as the reference's own runner is at startup.
  paramTable.selectParameterTable(paramTable.DEFAULT_TABLE_TYPE);
  emit(`GATE ${json(report)} ${json(tableGate.evaluateTableGate(report))}`);
  count("GATE");
}

// --- 7. The /vcu-write request parser and the UTC minute --------------------------------------
const nowForParse = Date.UTC(2026, 7, 16, 14, 3, 12);
const minute = httpWrite.utcMinute(nowForParse);
for (let at = 0; at < 3000; at++) {
  const params = new URLSearchParams();
  const action = pick(["parameter", "bit", "parameters", "read-service-stamp", "set-service-point", "sync-clock", "clear-dtcs", "charge-soc-limit", "charge-soc-limit-read", "reset-vcu", "nonsense", ""]);
  if (action) params.set("action", action);
  if (chance(0.7)) params.set("name", pick([...curatedNames, "NOPE"]));
  if (chance(0.7)) params.set("value", String(between(-300, 300)));
  if (chance(0.5)) params.set("value", pick(["0x50", "-0x50", "0x1113", "nope"]));
  if (chance(0.6)) params.set("expected", String(between(-300, 300)));
  if (chance(0.5)) params.set("bit", pick(["heated-handlebars", "nope"]));
  if (chance(0.5)) params.set("on", pick(["0", "1", "2"]));
  if (chance(0.5)) params.set("pct", pick([String(between(-10, 110)), "nope"]));
  if (chance(0.6)) {
    const confirm = pick([action, `${action}-${params.get("pct") ?? ""}`, "charge-soc-limit-off", minute, "2020-01-01T00:00Z", `charge-soc-limit-${params.get("pct") ?? ""}`, "wrong"]);
    params.set("confirm", confirm);
  }
  for (let batch = between(0, 3); batch > 0; batch--) {
    params.append("w", pick([`${pick(curatedNames)}:${between(0, 100)}:${between(0, 100)}`, "bad", `X::`, `${pick(curatedNames)}:nope:0`]));
  }
  emit(`PARSE ${json(params.toString())} ${json(httpWrite.parseWriteRequest(params, nowForParse))}`);
  count("PARSE");
  // parseStatusRequest on the same query.
  emit(`SREQ ${json(params.toString())} ${json(httpWrite.parseStatusRequest(params))}`);
  count("SREQ");
}
for (let at = 0; at < 200; at++) {
  const epoch = Date.UTC(2020 + between(0, 20), between(0, 11), between(1, 28), between(0, 23), between(0, 59), between(0, 59), between(0, 999));
  emit(`MIN ${epoch} ${json(httpWrite.utcMinute(epoch))}`);
  count("MIN");
}

// --- 8. The charge-command frames (stage 5) ---------------------------------------------------
for (let at = 0; at < 2000; at++) {
  const mode = pick(["ac", "dc"]);
  const ceiling = between(0, 260);
  const selected = between(-5, 90);
  let frames: string | null = null;
  try {
    frames = (chargeCommand.buildChargeCurrentCommand(mode, selected, ceiling) as { id: number; data: Uint8Array }[])
      .map(f => `${f.id.toString(16)}:${hex(f.data)}`)
      .join(",");
  } catch (err) {
    frames = `ERR ${(err as Error).message}`;
  }
  emit(`CC ${mode} ${selected} ${ceiling} ${frames}`);
  count("CC");
}
{
  const frames = (chargeCommand.buildChargeStopCommand() as { id: number; data: Uint8Array }[])
    .map(f => `${f.id.toString(16)}:${hex(f.data)}`)
    .join(",");
  emit(`CS ${frames}`);
  count("CS");
}

// --- 9. The charge-ack judge (stage 5) --------------------------------------------------------
for (let at = 0; at < 3000; at++) {
  const commandedAmps = between(0, 80);
  const sentAtMs = between(0, 100000);
  const samples: { atMs: number; amps: number }[] = [];
  for (let count = between(0, 10); count > 0; count--) {
    samples.push({ atMs: sentAtMs + between(-70000, 15000), amps: between(0, 90) });
  }
  samples.sort((a, b) => a.atMs - b.atMs);
  const nowMs = sentAtMs + between(0, 15000);
  const supersededAtMs = chance(0.2) ? sentAtMs + between(-2000, 12000) : null;
  const input = { commandedAmps, sentAtMs, nowMs, samples, supersededAtMs };
  const verdict = acknowledge.judgeChargeCommand(input);
  emit(`ACK ${json(input)} ${json(verdict)} ${acknowledge.CHARGE_ACK_CODE[verdict.kind]} ${json(acknowledge.describeChargeAck(verdict, commandedAmps))}`);
  count("ACK");
}

await writeFile(outputPath, lines.join("\n") + "\n");
process.stderr.write(
  "golden-write: " +
    Object.entries(counts)
      .map(([kind, n]) => `${kind}=${n}`)
      .join(" ") +
    "\n"
);
