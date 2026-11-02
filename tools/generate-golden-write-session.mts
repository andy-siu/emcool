// Drives cool-eva's OWN write-session code — src/vcu/write-session.ts's writeParameter,
// writeParameters, readServiceStamp, setServicePoint, clearStoredDtcs and resetVcu — against a
// scripted KWP/OBD bus on a fake clock, and records the frames it sent and the outcome it reached,
// for host/tests/check_write_session.cpp to hold the C++ WriteSession to. This is the stateful half
// of stage 4: the session timing (300 ms reply window, 10 ms pace, the security cooldown), the
// compare-and-swap, the unlock handshake, the read-back, and every refusal path.
//
// The scripted bus answers a request by its service byte, in the sequence the scenario lists; a
// reply of "-" is silence (the session's timeout fires). The C++ side replays the identical script.
//
//   node tools/generate-golden-write-session.mts <cool-eva checkout> <output file>
//
// Lines:
//   case <n> <op> <micro|-> <arg…>     op = param|batch|stamp|servicepoint|clear|reset
//   reply <requestHex> <replyHex|->     one scripted reply, in order of the requests expected
//   t <ms> <id-hex> <dataHex>           a frame cool-eva transmitted (the assertion)
//   outcome <json>                      the resolved outcome (the assertion)
//   end

import { registerHooks } from "node:module";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [coolEvaDirectory, outputPath] = process.argv.slice(2);
if (!coolEvaDirectory || !outputPath) {
  process.stderr.write("usage: node tools/generate-golden-write-session.mts <cool-eva checkout> <output file>\n");
  process.exit(2);
}
const coolEvaRoot = pathToFileURL(resolve(coolEvaDirectory)).href + "/";
const srcRoot = coolEvaRoot + "src/";

const fakeDirectory = await mkdtemp(join(tmpdir(), "emcool-ws-"));
const fakeMonotonicPath = join(fakeDirectory, "fake-monotonic.mjs");
await writeFile(
  fakeMonotonicPath,
  `export function monotonicNow() { return (globalThis).__now; }
export function since(mark) { return (globalThis).__now - mark; }
`
);
const fakeMonotonicUrl = pathToFileURL(fakeMonotonicPath).href;
// The charge-current runner cases reach write-audit.ts (fs) and version.ts; stub both in memory.
const fakeFsPath = join(fakeDirectory, "fake-fs.mjs");
await writeFile(
  fakeFsPath,
  `const files = () => ((globalThis).__fakeFiles ??= new Map());
export async function mkdir() {}
export async function readFile(path) { if (!files().has(path)) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" }); return files().get(path); }
export async function appendFile(path, data) { files().set(path, (files().get(path) ?? "") + String(data)); }
export async function writeFile(path, data) { files().set(path, String(data)); }
export async function rm() {} export async function rename() {} export async function readdir() { return []; }
export async function stat(path) { return { size: (files().get(path) ?? "").length }; }
export default { mkdir, readFile, appendFile, writeFile, rm, rename, readdir, stat };
`
);
const fakeDurablePath = join(fakeDirectory, "fake-durable.mjs");
await writeFile(
  fakeDurablePath,
  `const files = () => ((globalThis).__fakeFiles ??= new Map());
export async function appendDurably(path, data) { files().set(path, (files().get(path) ?? "") + String(data)); }
export async function openDeferredAppend(path) { if (!files().has(path)) files().set(path, ""); return { write: async d => files().set(path, files().get(path) + String(d)), close: async () => {} }; }
export async function replaceFileDurably(path, data) { files().set(path, String(data)); }
export async function syncDirectory() {}
export function durabilityCounters() { return { flushes: 0, directorySyncs: 0 }; }
`
);
const fakeVersionPath = join(fakeDirectory, "fake-version.mjs");
await writeFile(fakeVersionPath, `export async function readRunningVersion() { return { commit: null, dirty: false, trustworthy: true, label: "test" }; }\n`);
const fakeFsUrl = pathToFileURL(fakeFsPath).href;
const fakeDurableUrl = pathToFileURL(fakeDurablePath).href;
const fakeVersionUrl = pathToFileURL(fakeVersionPath).href;
registerHooks({
  resolve(specifier, context, nextResolve) {
    const parent = context.parentURL ?? "";
    if (parent.startsWith(srcRoot) && (specifier === "fs/promises" || specifier === "node:fs/promises")) {
      return { url: fakeFsUrl, shortCircuit: true, format: "module" };
    }
    const result = nextResolve(specifier, context);
    if (parent.startsWith(srcRoot) && result.url.startsWith(srcRoot + "monotonic.ts")) {
      return { url: fakeMonotonicUrl, shortCircuit: true, format: "module" };
    }
    if (parent.startsWith(srcRoot) && result.url.startsWith(srcRoot + "storage/durable.ts")) {
      return { url: fakeDurableUrl, shortCircuit: true, format: "module" };
    }
    if (parent.startsWith(srcRoot) && result.url.startsWith(srcRoot + "version.ts")) {
      return { url: fakeVersionUrl, shortCircuit: true, format: "module" };
    }
    if (parent.startsWith(srcRoot) && (result.url.startsWith(srcRoot) || result.url.includes("/scripts/")) && !result.url.includes("?")) {
      const tag = new URL(parent).searchParams.get("gen");
      if (tag !== null) return { ...result, url: `${result.url}?gen=${tag}` };
    }
    return result;
  },
});

const lines: string[] = [];
let now = 0;
const emit = (line: string) => void lines.push(line);
const hex = (bytes: ArrayLike<number>) => Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
(globalThis as any).__now = 0;

// A single timer queue on the fake clock, as generate-golden-obd.mts.
class Queue {
  items: { at: number; seq: number; cb: () => void; cancelled: boolean }[] = [];
  push(item: { at: number; seq: number; cb: () => void; cancelled: boolean }) {
    this.items.push(item);
    this.items.sort((a, b) => a.at - b.at || a.seq - b.seq);
  }
  live() {
    while (this.items[0]?.cancelled) this.items.shift();
    return this.items[0];
  }
  pop() {
    return this.items.shift();
  }
}
let timers = new Queue();
let seq = 0;
const coerce = (delay: unknown) => {
  const after = Number(delay);
  return after >= 1 && after <= 2_147_483_647 ? after : 1;
};
globalThis.setTimeout = ((cb: (...a: unknown[]) => void, delay?: number, ...a: unknown[]) => {
  const timer = { at: now + coerce(delay), seq: seq++, cb: () => cb(...a), cancelled: false };
  timers.push(timer);
  return timer as unknown as NodeJS.Timeout;
}) as unknown as typeof setTimeout;
globalThis.clearTimeout = ((t: { cancelled: boolean } | undefined) => {
  if (t) t.cancelled = true;
}) as typeof clearTimeout;
Object.defineProperty(performance, "now", { value: () => now, configurable: true, writable: true });
Date.now = () => 1790000000000 + now;
console.log = console.warn = console.error = () => {};

const realSetImmediate = globalThis.setImmediate;
const drain = async () => {
  for (let turn = 0; turn < 6; turn++) await new Promise<void>(done => realSetImmediate(done));
};

let generation = 0;
const importFresh = (path: string) => import(`${coolEvaRoot}${path}?gen=${generation}`);

// The scripted bus: a list of (requestHex → replyHex|null) in order. Each send looks up the next
// scripted reply whose request matches by its KWP service byte / OBD mode; a null reply is silence.
interface Script {
  replies: { match: (id: number, data: Uint8Array) => boolean; reply: number[] | null; delayMs: number }[];
  used: boolean[];
  session: { handleFrame: (id: number, data: Buffer) => boolean } | null;
}

function makeChannel(script: Script) {
  return {
    send(message: { id: number; data: Buffer }) {
      emit(`t ${now} ${message.id.toString(16)} ${hex(message.data)}`);
      const data = Uint8Array.from(message.data);
      for (let at = 0; at < script.replies.length; at++) {
        if (script.used[at]) continue;
        const entry = script.replies[at];
        if (!entry.match(message.id, data)) continue;
        script.used[at] = true;
        if (entry.reply === null) return message.data.length;
        const replyId = kwpReplyId(message.id);
        const reply = entry.reply;
        const timer = { at: now + entry.delayMs, seq: seq++, cb: () => {
          emit(`r ${now} ${replyId.toString(16)} ${hex(reply)}`);
          script.session?.handleFrame(replyId, Buffer.from(reply));
        }, cancelled: false };
        timers.push(timer);
        return message.data.length;
      }
      return message.data.length;
    },
  };
}
function kwpReplyId(requestId: number): number {
  if (requestId === 0x7c0) return 0x7e0;  // KWP
  if (requestId === 0x7df) return 0x7e8;  // OBD
  return 0x7e0;
}

async function runToIdle(): Promise<void> {
  while (true) {
    const due = timers.live();
    if (!due) break;
    now = due.at;
    (globalThis as any).__now = now;
    timers.pop();
    due.cb();
    await drain();
  }
}

// ---- scenarios -------------------------------------------------------------------------------

type Micro = "A8" | "A9";

// Build a reply list from a terse scenario spec and emit the `reply` lines so the C++ replays them.
function script(entries: { onService?: number; onPid?: number; onClear?: boolean; reply: number[] | null; delayMs?: number }[]): Script {
  const replies = entries.map(entry => ({
    match: (id: number, data: Uint8Array) => {
      if (entry.onClear) return id === 0x7df && data[1] === 0x04;
      if (entry.onPid !== undefined) return id === 0x7df && data[1] === 0x01 && data[2] === entry.onPid;
      if (entry.onService !== undefined) return id === 0x7c0 && data[2] === entry.onService;
      return false;
    },
    reply: entry.reply,
    delayMs: entry.delayMs ?? 2,
  }));
  return { replies, used: entries.map(() => false), session: null };
}

async function main(): Promise<void> {
  // The modules, fresh per scenario so the security cooldown map resets.
  const scenarios: (() => Promise<void>)[] = [];

  // A full write that is accepted and reads back: session(50 81), read-before(=prev), seed, key, 2E, read-back(=new).
  const identifierOf = (index: number) => 0x1000 | index;
  const idHi = (index: number) => (identifierOf(index) >> 8) & 0xff;
  const idLo = (index: number) => identifierOf(index) & 0xff;

  const plan = async (name: string, value: number, previous: number, replies: Script, op: "ok" | "mismatch" | "stale") => {
    generation++;
    timers = new Queue();
    seq = 0;
    now = 0;
    (globalThis as any).__now = 0;
    const writeTargets = await importFresh("src/vcu/write-targets.ts");
    const writeSession = await importFresh("src/vcu/write-session.ts");
    const paramTable = await importFresh("src/vcu/param-table.ts");
    // A confirmed TABLE_TYPE report for the active (default) table, so the codec's table gate opens.
    const tableType = paramTable.DEFAULT_TABLE_TYPE;
    const report = { verdicts: [{ index: 276, value: tableType }, { index: 277, value: tableType }], unusable: [] };
    const planned = writeTargets.planWrite(name, value, previous);
    if (!planned.ok) {
      emit(`case ${scenarios.length} planfail ${name} ${value} ${previous}`);
      emit(`outcome ${JSON.stringify({ planfail: planned.reason })}`);
      emit("end");
      return;
    }
    emit(`case ${scenarios.length} param ${planned.plan.micro} ${name} ${value} ${previous}`);
    const { session, finished } = writeSession.writeParameter(makeChannel(replies), planned.plan, report);
    replies.session = session;
    const settled = finished.then((outcome: unknown) => emit(`outcome ${JSON.stringify(outcome)}`));
    await drain();
    await runToIdle();
    await settled;
    emit("end");
  };

  // Scenario set — the shapes cool-eva's own checks exercise.
  // 1. accepted
  scenarios.push(() =>
    plan("MAX_DC_CHG_CURRENT", 80, 75, script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x4b] },
      { onService: 0x27, reply: [0xf1, 0x06, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78] },
      // 27 02: the key. accept.
      { onService: 0x27, reply: [0xf1, 0x02, 0x67, 0x02] },
      { onService: 0x2e, reply: [0xf1, 0x03, 0x6e, idHi(258), idLo(258)] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x50] },
    ]), "ok")
  );
  // 2. read-back mismatch (reads 0x4b, not 0x50)
  scenarios.push(() =>
    plan("MAX_DC_CHG_CURRENT", 80, 75, script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x4b] },
      { onService: 0x27, reply: [0xf1, 0x06, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78] },
      { onService: 0x27, reply: [0xf1, 0x02, 0x67, 0x02] },
      { onService: 0x2e, reply: [0xf1, 0x03, 0x6e, idHi(258), idLo(258)] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x4b] },
    ]), "mismatch")
  );
  // 3. stale precondition (reads 0x4a, not the 75 the caller believed)
  scenarios.push(() =>
    plan("MAX_DC_CHG_CURRENT", 80, 75, script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x4a] },
    ]), "stale")
  );
  // 4. refused at the key (0x35 invalidKey)
  scenarios.push(() =>
    plan("MAX_DC_CHG_CURRENT", 80, 75, script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, idHi(258), idLo(258), 0x4b] },
      { onService: 0x27, reply: [0xf1, 0x06, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78] },
      { onService: 0x27, reply: [0xf1, 0x03, 0x7f, 0x27, 0x35] },
    ]), "ok")
  );
  // 5. the micro never answers 10 81 (silent session)
  scenarios.push(() =>
    plan("MAX_DC_CHG_CURRENT", 80, 75, script([{ onService: 0x10, reply: null }]), "ok")
  );
  // 6. a bad plan (out of range) — refused before any frame
  scenarios.push(() => plan("MAX_DC_CHG_CURRENT", 999, 75, script([]), "ok"));

  // 7. the service stamp read: four WORDs off A8.
  scenarios.push(async () => {
    generation++;
    timers = new Queue();
    seq = 0;
    now = 0;
    (globalThis as any).__now = 0;
    const writeSession = await importFresh("src/vcu/write-session.ts");
    const replies = script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, 0x13, 0xe8, 0x00, 0x00] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, 0x13, 0xe9, 0x00, 0x00] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, 0x13, 0xea, 0x4d, 0x2e] },
      { onService: 0x22, reply: [0xf1, 0x04, 0x62, 0x13, 0xeb, 0x00, 0x00] },
    ]);
    emit(`case ${scenarios.length} stamp A8`);
    const { session, finished } = writeSession.readServiceStamp(makeChannel(replies), Date.now());
    replies.session = session;
    const settled = finished.then((outcome: unknown) => emit(`outcome ${JSON.stringify(outcome)}`));
    await drain();
    await runToIdle();
    await settled;
    emit("end");
  });

  // 8. reset-vcu: A9 then A8 both answer 50 81, then both 51.
  scenarios.push(async () => {
    generation++;
    timers = new Queue();
    seq = 0;
    now = 0;
    (globalThis as any).__now = 0;
    const writeSession = await importFresh("src/vcu/write-session.ts");
    const replies = script([
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x10, reply: [0xf1, 0x02, 0x50, 0x81] },
      { onService: 0x11, reply: [0xf1, 0x02, 0x51, 0x02] },
      { onService: 0x11, reply: [0xf1, 0x02, 0x51, 0x02] },
    ]);
    emit(`case ${scenarios.length} reset -`);
    const { session, finished } = writeSession.resetVcu(makeChannel(replies));
    replies.session = session;
    const settled = finished.then((outcome: unknown) => emit(`outcome ${JSON.stringify(outcome)}`));
    await drain();
    await runToIdle();
    await settled;
    emit("end");
  });

  // 9. clear-dtcs: Mode 04 answered 44.
  scenarios.push(async () => {
    generation++;
    timers = new Queue();
    seq = 0;
    now = 0;
    (globalThis as any).__now = 0;
    const writeSession = await importFresh("src/vcu/write-session.ts");
    const replies = script([{ onClear: true, reply: [0x01, 0x44] }]);
    emit(`case ${scenarios.length} clear -`);
    const { session, finished } = writeSession.clearStoredDtcs(makeChannel(replies));
    replies.session = session;
    const settled = finished.then((outcome: unknown) => emit(`outcome ${JSON.stringify(outcome)}`));
    await drain();
    await runToIdle();
    await settled;
    emit("end");
  });

  // The charge-current and charge-stop runner actions: driven through cool-eva's createVcuWriteRunner
  // with seeded live signals and a fake channel, recording the frames and the ServiceWriteAnswer.
  const chargeCase = async (
    label: string,
    seed: Record<string, number>,
    request: { kind: "charge-current"; amps: number; origin: "manual" } | { kind: "charge-stop" }
  ) => {
    generation++;
    timers = new Queue();
    seq = 0;
    now = 0;
    (globalThis as any).__now = 0;
    const signals = await importFresh("src/can/signals.ts");
    const writeRunner = await importFresh("src/vcu/write-runner.ts");
    for (const [key, value] of Object.entries(seed)) signals.record(key, value, 1790000000000 + now);
    const sent: string[] = [];
    const channel = {
      send(message: { id: number; data: Buffer }) {
        emit(`t ${now} ${message.id.toString(16)} ${hex(message.data)}`);
        sent.push(message.id.toString(16));
      },
    };
    const runner = writeRunner.createVcuWriteRunner({
      channel: () => channel,
      busIsActive: true,
      enabled: true,
      directory: "/vcu-write",
      gate: () => ({ safe: true, blockers: [], checks: [], chargingEvidence: "the charge manager reports a live session" }),
      holdPoller: () => Promise.resolve({ release: () => {} }),
      latestSweep: async () => null,
    });
    const seedText = Object.entries(seed).map(([k, v]) => `${k}=${v}`).join(",") || "-";
    emit(`case ${scenarios.length} ${label} ${seedText} ${request.kind === "charge-current" ? request.amps : 0}`);
    const settled = runner.perform(request).then((answer: unknown) => emit(`chargeanswer ${JSON.stringify(answer)}`));
    await drain();
    await runToIdle();
    await settled;
    emit("end");
  };
  // DC session, ceiling present: 50 A accepted.
  scenarios.push(() => chargeCase("chargecurrent", { charge_manager_state: 0x23, fast_dc_limit_max_a: 75 }, { kind: "charge-current", amps: 50, origin: "manual" }));
  // AC session, no ceiling broadcast: falls back to 15.
  scenarios.push(() => chargeCase("chargecurrent", { charge_manager_state: 0x02 }, { kind: "charge-current", amps: 10, origin: "manual" }));
  // No session: refused.
  scenarios.push(() => chargeCase("chargecurrent", {}, { kind: "charge-current", amps: 10, origin: "manual" }));
  // Over the ceiling: refused.
  scenarios.push(() => chargeCase("chargecurrent", { charge_manager_state: 0x23, fast_dc_limit_max_a: 40 }, { kind: "charge-current", amps: 60, origin: "manual" }));
  // Charge stop, live session.
  scenarios.push(() => chargeCase("chargestop", { charge_manager_state: 0x02 }, { kind: "charge-stop" }));
  // Charge stop, no session: refused.
  scenarios.push(() => chargeCase("chargestop", {}, { kind: "charge-stop" }));

  for (const run of scenarios) await run();
  await writeFile(outputPath, lines.join("\n") + "\n");
  process.stderr.write(`golden-write-session: ${scenarios.length} scenarios, ${lines.filter(l => l.startsWith("t ")).length} frames sent\n`);
}

await main();
