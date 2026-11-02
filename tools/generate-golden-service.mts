// Drives cool-eva's service mode — src/vcu/read-runner.ts with its sweeps and probes, the OBD poller
// beside it as in production — against cool-eva's own simulated VCU micros
// (scripts/simulated-vcu-micro.ts) and a scripted OBD ECU, on a fake clock, and writes what it did
// for host/tests/check_service.cpp to replay through the C++ port.
//
// The runs cover: sweeps that complete, that are cancelled and then resumed from the partial file,
// that stop when the bike moves or its broadcasts go stale; micros that are silent, refuse indices,
// answer the wrong width, stall after a first frame, or let a session lapse; the A8 rows that need
// the OBD poller parked; TABLE_TYPEs carried, uncarried and split; probes of every bank, refused
// while a sweep runs or the bike is unsafe; and the bodies of /vcu-read, /vcu-params and /vcu-backup.
//
//   node tools/generate-golden-service.mts <cool-eva checkout> <output file>
//
// The same timing rules as generate-golden-obd.mts: whole milliseconds; in each, scheduled actions,
// then cool-eva's due timers, then the ECUs' (frames arriving). The simulated micros' setTimeout is
// rewritten, on load, into the ECUs' queue. cool-eva's fs/promises and storage/durable.ts are
// replaced, for the modules under test, by an in-memory store that resolves at once.
//
// Lines, besides generate-golden-obd.mts' r/t/s/l:
//   run <n> <wall clock base ms> <end ms> <bus active 0|1> <service mode enabled 0|1> <directory>
//   seed <json string>                   the resume file an earlier sweep left, before the run starts
//   g <ms> <key> <value>                 the scenario's own record() of a gate signal (an input)
//   a <ms> <action…>                     start | cancel | probe <target> <bank> <index> | state |
//                                        gate | params | backup | expire <micro> | lifetime |
//                                        freeze | lifetime-stats | freeze-frames | expected-get |
//                                        expected-set <component> <symptom> <expected>
//   j <ms> <what> <json>                 an action's answer (a probe's when it resolves); for the
//                                        actions answered by cool-eva's HTTP handlers,
//                                        j <ms> <what> <status> <body>
//   end

import { registerHooks } from "node:module";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [coolEvaDirectory, outputPath] = process.argv.slice(2);
if (!coolEvaDirectory || !outputPath) {
  process.stderr.write("usage: node tools/generate-golden-service.mts <cool-eva checkout> <output file>\n");
  process.exit(2);
}
const coolEvaRoot = pathToFileURL(resolve(coolEvaDirectory)).href + "/";
const srcRoot = coolEvaRoot + "src/";
const scriptsRoot = coolEvaRoot + "scripts/";

// The in-memory filesystem, as two small modules standing in for fs/promises and durable.ts.
const fakeDirectory = await mkdtemp(join(tmpdir(), "emcool-fake-fs-"));
const fakeFsPath = join(fakeDirectory, "fake-fs.mjs");
const fakeDurablePath = join(fakeDirectory, "fake-durable.mjs");
await writeFile(
  fakeFsPath,
  `const files = () => globalThis.__fakeFiles;
const missing = path => Object.assign(new Error("ENOENT: no such file " + path), { code: "ENOENT" });
export async function mkdir() {}
export async function readFile(path) { if (!files().has(path)) throw missing(path); return files().get(path); }
export async function rm(path) { files().delete(path); }
export async function rename(from, to) { if (!files().has(from)) throw missing(from); files().set(to, files().get(from)); files().delete(from); }
export async function readdir() { return []; }
export async function stat(path) { if (!files().has(path)) throw missing(path); return { size: files().get(path).length }; }
export async function writeFile(path, data) { files().set(path, String(data)); }
export async function appendFile(path, data) { files().set(path, (files().get(path) ?? "") + String(data)); }
export default { mkdir, readFile, rm, rename, readdir, stat, writeFile, appendFile };
`
);
await writeFile(
  fakeDurablePath,
  `const files = () => globalThis.__fakeFiles;
export async function appendDurably(path, data) { files().set(path, (files().get(path) ?? "") + String(data)); }
export async function openDeferredAppend(path) {
  if (!files().has(path)) files().set(path, "");
  return { write: async data => { files().set(path, files().get(path) + String(data)); }, close: async () => {} };
}
export async function replaceFileDurably(path, data) { files().set(path, String(data)); }
export async function syncDirectory() {}
export function durabilityCounters() { return { flushes: 0, directorySyncs: 0 }; }
`
);
const fakePerfPath = join(fakeDirectory, "fake-perf-hooks.mjs");
await writeFile(
  fakePerfPath,
  `export function monitorEventLoopDelay() { return { enable() {}, disable() {}, count: 0, max: 0 }; }
export const performance = globalThis.performance;
`
);
const fakePerfUrl = pathToFileURL(fakePerfPath).href;
const fakeFsUrl = pathToFileURL(fakeFsPath).href;
const fakeDurableUrl = pathToFileURL(fakeDurablePath).href;

let generation = 0;
registerHooks({
  resolve(specifier, context, nextResolve) {
    const parent = context.parentURL ?? "";
    const fromCoolEva = parent.startsWith(srcRoot) || parent.startsWith(scriptsRoot);
    if (fromCoolEva && (specifier === "fs/promises" || specifier === "node:fs/promises")) {
      return { url: fakeFsUrl, shortCircuit: true, format: "module" };
    }
    // cool-eva samples its event loop's delay; emcool has none to sample, and says so.
    if (fromCoolEva && (specifier === "node:perf_hooks" || specifier === "perf_hooks")) {
      return { url: fakePerfUrl, shortCircuit: true, format: "module" };
    }
    const result = nextResolve(specifier, context);
    if (fromCoolEva && result.url.startsWith(srcRoot + "storage/durable.ts")) {
      return { url: fakeDurableUrl, shortCircuit: true, format: "module" };
    }
    if (fromCoolEva && (result.url.startsWith(srcRoot) || result.url.startsWith(scriptsRoot)) && !result.url.includes("?")) {
      const tag = new URL(parent).searchParams.get("gen");
      if (tag !== null) return { ...result, url: `${result.url}?gen=${tag}` };
    }
    return result;
  },
  load(url, context, nextLoad) {
    const result = nextLoad(url, context);
    if (url.startsWith(srcRoot + "can/signals.ts")) {
      const source = String(result.source);
      const marker = "export function record(key: string, value: number, ts: number = Date.now()): void {";
      if (!source.includes(marker)) throw new Error("cool-eva's record() has changed shape; update the hook");
      return { ...result, source: source.replace(marker, `${marker} (globalThis as any).__emcoolRecorded(key, value);`) };
    }
    if (url.startsWith(scriptsRoot + "simulated-vcu-micro.ts")) {
      // The micros' replies go into the ECUs' queue, after cool-eva's own timers each millisecond.
      return { ...result, source: String(result.source).replace(/\bsetTimeout\(/g, "(globalThis as any).__ecuTimeout(") };
    }
    return result;
  },
});
const importFresh = (path: string) => import(`${coolEvaRoot}${path}?gen=${generation}`);

// ---------------------------------------------------------------------------------------------
// The clock, the two timer queues, and the output.

const lines: string[] = [];
let now = 0;
let wallBase = 0;
const emit = (line: string) => void lines.push(line);
const hex = (bytes: ArrayLike<number>) => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
Object.defineProperty(performance, "now", { value: () => now, configurable: true, writable: true });
Date.now = () => wallBase + now;

class Queue<T extends { at: number; seq: number }> {
  items: T[] = [];
  push(item: T) {
    this.items.push(item);
    this.items.sort((left, right) => left.at - right.at || left.seq - right.seq);
  }
  peek(): T | undefined {
    return this.items[0];
  }
  pop(): T | undefined {
    return this.items.shift();
  }
}
interface FakeTimer {
  at: number;
  seq: number;
  callback: () => void;
  cancelled: boolean;
  interval: number;
}
let timers = new Queue<FakeTimer>();
let ecuTimers = new Queue<FakeTimer>();
let timerSeq = 0;
const coerce = (delay: unknown) => {
  const after = Number(delay);
  return after >= 1 && after <= 2_147_483_647 ? after : 1;
};
function makeTimer(queue: Queue<FakeTimer>, callback: () => void, delay: unknown, interval: number) {
  const timer: FakeTimer & { unref(): unknown; ref(): unknown; hasRef(): boolean } = {
    at: now + coerce(delay),
    seq: timerSeq++,
    callback,
    cancelled: false,
    interval,
    unref: () => timer,
    ref: () => timer,
    hasRef: () => true,
  };
  queue.push(timer);
  return timer;
}
globalThis.setTimeout = ((callback: (...args: unknown[]) => void, delay?: number, ...args: unknown[]) =>
  makeTimer(timers, () => callback(...args), delay, 0)) as unknown as typeof setTimeout;
globalThis.setInterval = ((callback: (...args: unknown[]) => void, delay?: number, ...args: unknown[]) =>
  makeTimer(timers, () => callback(...args), delay, coerce(delay))) as unknown as typeof setInterval;
globalThis.clearTimeout = ((timer: FakeTimer | undefined) => {
  if (timer) timer.cancelled = true;
}) as typeof clearTimeout;
globalThis.clearInterval = globalThis.clearTimeout as unknown as typeof clearInterval;
(globalThis as any).__ecuTimeout = (callback: () => void, delay: number) => makeTimer(ecuTimers, callback, delay, 0);

const realSetImmediate = globalThis.setImmediate;
const drain = async () => {
  // Enough turns for promise chains through the fake filesystem to settle.
  for (let turn = 0; turn < 4; turn++) await new Promise<void>(done => realSetImmediate(done));
};

let scenarioRecording = false;
(globalThis as any).__emcoolRecorded = (key: string, value: number) =>
  emit(`${scenarioRecording ? "g" : "s"} ${now} ${key} ${value}`);
const logLine = (level: string) => (...args: unknown[]) => emit(`l ${now} ${level} ${String(args[0])}`);
console.log = logLine("I");
console.info = logLine("I");
console.warn = logLine("W");
console.error = logLine("E");

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
const random = mulberry32(0x5e17);
const chance = (probability: number) => random() < probability;
const between = (low: number, high: number) => low + Math.floor(random() * (high - low + 1));
const pick = <T,>(items: readonly T[]): T => items[Math.floor(random() * items.length)];
const randomBytes = (count: number) => Array.from({ length: count }, () => between(0, 255));

const DIRECTORY = "/vcu-params";
const GATE_BROADCAST = ["speed_can_kmh", "motor_rpm_can", "moving", "energized", "go", "go_request", "throttle_on"];

// ---------------------------------------------------------------------------------------------

async function runScenario(index: number, durationMs: number): Promise<void> {
  generation = index + 1;
  timers = new Queue<FakeTimer>();
  ecuTimers = new Queue<FakeTimer>();
  timerSeq = 0;
  now = 0;
  wallBase = 1_790_000_000_000 + index * 7_200_000;
  (globalThis as any).__fakeFiles = new Map<string, string>();

  const runnerModule = await importFresh("src/vcu/read-runner.ts");
  const obd = await importFresh("src/can/obd.ts");
  const signals = await importFresh("src/can/signals.ts");
  const paramTable = await importFresh("src/vcu/param-table.ts");
  const vcuParams = await importFresh("src/http/vcu-params.ts");
  const backup = await importFresh("src/vcu/backup-csv.ts");
  const catalogue = await importFresh("src/vcu/table-catalog.ts");
  const simulated = await importFresh("scripts/simulated-vcu-micro.ts");
  const http = {
    lifetimeRead: await importFresh("src/http/lifetime-read.ts"),
    freezeFrameRead: await importFresh("src/http/freeze-frame-read.ts"),
    lifetimeStats: await importFresh("src/http/lifetime-stats.ts"),
    freezeFrames: await importFresh("src/http/freeze-frames.ts"),
    expectedFaults: await importFresh("src/http/expected-faults.ts"),
  };
  const faultInfokeys = await importFresh("src/diagnostics/fault-infokeys.ts");
  const infokeyTable = await importFresh("src/diagnostics/infokey-table.ts");
  const busIsActive = chance(0.95);
  const serviceEnabled = chance(0.92);
  emit(`run ${index} ${wallBase} ${durationMs} ${busIsActive ? 1 : 0} ${serviceEnabled ? 1 : 0} ${DIRECTORY}`);

  // The micros: every index of the active table answers, with the right width mostly.
  const known: number[] = catalogue.KNOWN_TABLE_TYPES;
  const tableType = chance(0.7) ? paramTable.DEFAULT_TABLE_TYPE : chance(0.6) ? pick(known) : between(0x1000, 0x7fff);
  const micros = (["A9", "A8"] as const).map(target => {
    const records = new Map<number, Uint8Array>();
    for (const parameter of paramTable.parameterTable()) {
      if (parameter.micro !== target) continue;
      const width = chance(0.98) ? paramTable.recordLengthFor(parameter.type) : between(1, 5);
      records.set(parameter.index, Uint8Array.from(randomBytes(width)));
    }
    if (target === "A8") for (const row of [278, 279, 613, 622, 627, 1000, 1006]) if (chance(0.7)) records.set(row, Uint8Array.from(randomBytes(pick([2, 4, 1]))));
    const typeIndex = target === "A9" ? 276 : 277;
    const reported = chance(0.85) ? tableType : chance(0.5) ? pick(known) : between(0, 0xffff);
    records.set(typeIndex, Uint8Array.from([reported >> 8, reported & 0xff]));
    const liveRecords = new Map<number, Uint8Array>([[5, Uint8Array.from([0x12, 0x34])], [9, Uint8Array.from([1])]]);
    // The A8's freeze frames: the lifetime components, and whatever its stored list names.
    const freezeFrames = new Map<number, Uint8Array>();
    let storedDtcList: Uint8Array | undefined;
    if (target === "A8") {
      const shortlists = faultInfokeys.FAULT_INFOKEYS as { component: number; symptom: number; infokeys: number[] }[];
      const frameFor = (component: number) => {
        const options = shortlists.filter(entry => entry.component === component);
        const symptom = options.length > 0 && chance(0.9) ? pick(options).symptom : between(0, 15);
        const fields: number[] = [];
        for (const id of faultInfokeys.infokeysFor(component, symptom)?.infokeys ?? []) {
          const width = infokeyTable.infokeyWidth(infokeyTable.lookupInfokey(id).datatype);
          fields.push(...randomBytes(width));
        }
        const payload = [0x57, between(0, 2), component >> 8, component & 0xff, (symptom << 4) | 0x0d, ...fields, between(0, 40)];
        return Uint8Array.from(chance(0.05) ? payload.slice(0, between(1, payload.length)) : payload);
      };
      for (const component of [51, 52]) if (chance(0.9)) freezeFrames.set(component, frameFor(component));
      if (chance(0.85)) {
        const listed = [...new Set(Array.from({ length: between(0, 6) }, () => between(1, 63)))];
        const records = listed.flatMap(component => [0, component, 0x0d]);
        if (chance(0.2)) records.push(0, 0, 0);
        for (const component of listed) if (chance(0.85)) freezeFrames.set(component, frameFor(component));
        storedDtcList = Uint8Array.from([0x58, listed.length + (records.length / 3 > listed.length ? 1 : 0), ...records]);
      }
    }
    const silentIndices = [...records.keys()].filter(() => chance(0.02));
    const refusedIndices = [...records.keys()].filter(() => chance(0.02));
    return {
      target,
      records,
      liveRecords,
      silentIndices,
      refusedIndices,
      freezeFrames,
      storedDtcList,
      stallsAfterFirstFrame: chance(0.05),
      sendsRequestFlowControl: chance(0.9),
    };
  });
  // Sometimes an earlier sweep's resume file, holding all but a few dozen A9 rows: the sweep then
  // reaches the A8 around the time its session from the opening ping lapses.
  if (chance(0.6)) {
    const snapshot = await importFresh("src/vcu/snapshot.ts");
    const targets = (await importFresh("src/vcu/sweep-targets.ts")).sweepTargets();
    const a9 = targets.filter((target: { micro: string }) => target.micro === "A9");
    const keep = a9.slice(0, Math.max(0, a9.length - between(55, 78)));
    const seeded = keep
      .map((target: { micro: string; index: number }) => {
        const record = micros.find(micro => micro.target === target.micro)?.records.get(target.index) ?? Uint8Array.from([0]);
        const row = snapshot.toParameterRow({ micro: target.micro, index: target.index, identifier: 0x1000 | target.index, status: "read", record, flowControlLatency: null });
        return JSON.stringify({ at: wallBase - 60_000, ...row }) + "\n";
      })
      .join("");
    (globalThis as any).__fakeFiles.set(`${DIRECTORY}/sweep.partial.jsonl`, seeded);
    emit(`seed ${JSON.stringify(seeded)}`);
  }
  const silentMicro = chance(0.1) ? pick(["A9", "A8"]) : null;
  const bus = simulated.simulateVcuMicros(micros.filter(micro => micro.target !== silentMicro));

  // The OBD side's ECU: answers PIDs, silent on trouble codes.
  const answersPids = chance(0.85);
  const arrive = (at: number, id: number, data: number[]) =>
    makeTimer(ecuTimers, () => deliver(id, Buffer.from(data)), at - now, 0);
  const sendFail = chance(0.15) ? 0.01 : 0;
  const channel = {
    send(message: { id: number; data: Buffer }) {
      const fail = chance(sendFail);
      emit(`t ${now} ${message.id.toString(16)} ${hex(message.data)} ${fail ? "fail" : "ok"}`);
      if (fail) throw new Error("simulated send failure");
      if (message.id === 0x7df && message.data[0] === 0x02 && message.data[1] === 0x01 && answersPids) {
        // Road speed agrees with the bike's broadcasts: zero unless it's moving.
        const pid = message.data[2];
        const a = pid === 0x0d ? (bike === "moving" ? 12 : 0) : between(0, 255);
        arrive(now + between(1, 25), 0x7e8, [0x04, 0x41, pid, a, between(0, 255), 0x55, 0x55, 0x55]);
      }
      if (message.id === 0x7c0) bus.channel.send(message);
      return message.data.length;
    },
  };
  const runner = runnerModule.createVcuReadRunner({ channel: () => channel, busIsActive, directory: DIRECTORY });
  function deliver(id: number, data: Buffer) {
    emit(`r ${now} ${id.toString(16)} ${hex(data) || "-"}`);
    if (obd.isObdResponse(id)) {
      if (runner.handleCanFrame(id, data, null)) return;
      obd.handleResponse(id, data);
    }
  }
  bus.channel.addListener("onMessage", (message: { id: number; data: Buffer }) => deliver(message.id, message.data));

  // The scenario: the bike's broadcasts, and what the dashboard asks.
  const actions: { at: number; run: () => void | Promise<void> }[] = [];
  let bike = "parked";
  const unsafeAt = chance(0.25) ? between(2000, durationMs - 2000) : null;
  const unsafeKind = pick(["moving", "stale", "energized"]);
  const charging = chance(0.15);
  for (let at = 1; at < durationMs; at += 250) {
    actions.push({
      at,
      run: () => {
        if (unsafeAt !== null && at >= unsafeAt && at < unsafeAt + 3000) bike = unsafeKind;
        else bike = "parked";
        if (bike === "stale") return;
        scenarioRecording = true;
        for (const key of GATE_BROADCAST) {
          const unsafe = (bike === "moving" && (key === "moving" || key === "speed_can_kmh")) || (bike === "energized" && key === "energized");
          signals.record(key, unsafe ? (key === "speed_can_kmh" ? 12.5 : 1) : 0);
        }
        if (charging) {
          signals.record("charge_manager_state", 0x23);
          signals.record("charge_manager_status", 0x08);
        }
        scenarioRecording = false;
      },
    });
  }
  const sweepAt = between(500, 3000) + (chance(0.5) ? 8000 : 0);
  const json = (value: unknown) => JSON.stringify(value);
  const act = (at: number, words: string, run: () => void | Promise<void>) =>
    actions.push({ at, run: async () => { emit(`a ${now} ${words}`); await run(); } });
  act(sweepAt, "start", () => emit(`j ${now} start ${json(runner.start())}`));
  if (chance(0.3)) act(sweepAt + between(1, 5), "start", () => emit(`j ${now} start ${json(runner.start())}`));
  if (chance(0.35)) {
    const cancelAt = sweepAt + between(500, 6000);
    act(cancelAt, "cancel", () => emit(`j ${now} cancel ${json(runner.cancel())}`));
    const restartAt = cancelAt + between(2000, 4000);
    act(restartAt, "start", () => emit(`j ${now} start ${json(runner.start())}`));
    // Started and stopped again before reading anything new: a snapshot that read exactly what the
    // last one did, which still replaces it.
    if (chance(0.5)) {
      const quickAt = restartAt + between(1500, 3000);
      act(quickAt, "cancel", () => emit(`j ${now} cancel ${json(runner.cancel())}`));
      act(quickAt + between(300, 800), "start", () => emit(`j ${now} start ${json(runner.start())}`));
      act(quickAt + between(801, 830), "cancel", () => emit(`j ${now} cancel ${json(runner.cancel())}`));
    }
  }
  let followUps = 0;
  for (let count = between(1, 4); count > 0; count--) {
    const target = pick(["A9", "A8", "A8", "a9"]);
    const bank = pick([1, 1, 2, 0, 15]);
    const probeIndex = pick([5, 9, 276, 277, 278, 622, 1006, 30, between(1, 400), 4095]);
    act(between(1000, durationMs - 1500), `probe ${target} ${bank} ${probeIndex}`, () => {
      const parsed = runnerModule && (globalThis as any).__parseProbe({ target, bank: String(bank), index: String(probeIndex) });
      if (!parsed.ok) {
        emit(`j ${now} probe ${json({ ok: false, reason: parsed.reason })}`);
        return;
      }
      void runner.probe(parsed.request).then((outcome: unknown) => {
        emit(`j ${now} probe ${json(outcome)}`);
        // Sometimes another probe of the same micro, around the 1.5 s session idle limit.
        if (followUps < 3 && chance(0.6)) {
          followUps++;
          const at = now + between(1380, 1620);
          const words = `probe ${target} ${bank} ${probeIndex}`;
          const position = actions.findIndex((action, at2) => at2 >= nextAction && action.at > at);
          const action = {
            at,
            run: async () => {
              emit(`a ${now} ${words}`);
              void runner.probe(parsed.request).then((again: unknown) => emit(`j ${now} probe ${json(again)}`));
            },
          };
          actions.splice(position === -1 ? actions.length : position, 0, action);
        }
      });
    });
  }
  for (let at = between(200, 2000); at < durationMs; at += between(800, 4000)) {
    act(at, "state", () => emit(`j ${now} state ${json(runner.state())}`));
    if (chance(0.3)) act(at, "gate", () => emit(`j ${now} gate ${json(runner.gate())}`));
  }
  if (chance(0.2)) act(between(3000, durationMs - 1000), `expire ${pick(["A9", "A8"])}`, () => {});
  // The freeze-frame reads and their files, through cool-eva's own handlers.
  const respond = (what: string) => {
    let status = 0;
    return {
      writeHead: (code: number) => {
        status = code;
      },
      end: (body?: Buffer | string) => emit(`j ${now} ${what} ${status} ${body === undefined ? "" : String(body)}`),
    };
  };
  const request = (method: string, header: string | null) => ({ method, headers: header === null ? {} : { "x-cool-eva": header } });
  const options = { runner, directory: DIRECTORY, enabled: serviceEnabled };
  // Some before the sweep starts, so they get the bus.
  if (chance(0.7)) {
    const first = pick(["lifetime", "freeze"]);
    act(between(50, 250), first, async () => {
      if (first === "lifetime") void http.lifetimeRead.handleLifetimeReadEndpoint(request("POST", "service-mode"), respond("lifetime"), options);
      else void http.freezeFrameRead.handleFreezeFrameReadEndpoint(request("POST", "service-mode"), respond("freeze"), options);
    });
  }
  for (let count = between(0, 3); count > 0; count--) {
    act(between(1500, durationMs - 3000), "lifetime", async () => {
      void http.lifetimeRead.handleLifetimeReadEndpoint(request("POST", "service-mode"), respond("lifetime"), options);
    });
  }
  for (let count = between(0, 2); count > 0; count--) {
    act(between(1500, durationMs - 3000), "freeze", async () => {
      void http.freezeFrameRead.handleFreezeFrameReadEndpoint(request("POST", "service-mode"), respond("freeze"), options);
    });
  }
  for (const at of [durationMs - 300, between(2000, durationMs - 400)]) {
    act(at, "lifetime-stats", async () => http.lifetimeStats.handleLifetimeStatsEndpoint(respond("lifetime-stats"), DIRECTORY));
    act(at + 1, "freeze-frames", async () => http.freezeFrames.handleFreezeFramesEndpoint(respond("freeze-frames"), DIRECTORY));
  }
  for (let count = between(0, 4); count > 0; count--) {
    const component = chance(0.85) ? String(between(1, 63)) : pick(["0", "64", "x", "1.5"]);
    const symptom = chance(0.9) ? String(between(0, 15)) : "16";
    const expected = chance(0.9) ? pick(["1", "1", "0"]) : "yes";
    const at = between(500, durationMs - 500);
    act(at, `expected-set ${component} ${symptom} ${expected}`, async () =>
      http.expectedFaults.handleExpectedFaultsEndpoint(
        request("POST", "expected-faults"),
        respond("expected"),
        new URL(`http://bike/expected-faults?component=${component}&symptom=${symptom}&expected=${expected}`),
        DIRECTORY
      )
    );
    act(at + 1, "expected-get", async () =>
      http.expectedFaults.handleExpectedFaultsEndpoint(request("GET", null), respond("expected"), new URL("http://bike/expected-faults"), DIRECTORY)
    );
  }
  for (const at of [durationMs - 400, between(sweepAt + 1000, durationMs - 500)]) {
    act(at, "params", async () => emit(`j ${now} params ${json(await vcuParams.loadLatestSnapshot(DIRECTORY))}`));
    act(at + 1, "backup", async () => {
      const latest = await vcuParams.loadLatestSnapshot(DIRECTORY);
      emit(`j ${now} backup ${json(latest.state === "snapshot" ? { rows: backup.exportableRowCount(latest.snapshot), csv: backup.snapshotToBackupCsv(latest.snapshot) } : { state: latest.state })}`);
    });
  }
  actions.sort((left, right) => left.at - right.at);
  // The expire actions reach into the bus; give them their effect.
  for (const action of actions) {
    const original = action.run;
    action.run = async () => {
      await original();
      const last = lines[lines.length - 1] ?? "";
      const match = /^a \d+ expire (A8|A9)$/.exec(last);
      if (match) {
        try {
          bus.expireSession(match[1]);
        } catch {
          // Never opened: nothing to expire.
        }
      }
    };
  }

  obd.initObd(channel);
  const stopObd = obd.startObdPoller(500);
  await drain();

  let nextAction = 0;
  while (true) {
    const live = (queue: Queue<FakeTimer>) => {
      while (queue.peek()?.cancelled) queue.pop();
      return queue.peek();
    };
    const candidates = [live(timers)?.at, live(ecuTimers)?.at, actions[nextAction]?.at].filter(
      (candidate): candidate is number => candidate !== undefined
    );
    if (candidates.length === 0) break;
    const at = Math.min(...candidates);
    if (at > durationMs) break;
    now = at;
    while (nextAction < actions.length && actions[nextAction].at === at) {
      await actions[nextAction++].run();
      await drain();
    }
    for (let due = live(timers); due !== undefined && due.at <= at; due = live(timers)) {
      timers.pop();
      due.callback();
      if (due.interval > 0 && !due.cancelled) {
        due.at = now + due.interval;
        due.seq = timerSeq++;
        timers.push(due);
      }
      await drain();
    }
    for (let due = live(ecuTimers); due !== undefined && due.at <= at; due = live(ecuTimers)) {
      ecuTimers.pop();
      due.callback();
      await drain();
    }
  }
  emit("end");
  stopObd();
  timers = new Queue<FakeTimer>();
  ecuTimers = new Queue<FakeTimer>();
}

const probe = await import(`${srcRoot}vcu/probe.ts`);
(globalThis as any).__parseProbe = probe.parseProbeRequest;
let simulated = 0;
for (let index = 0; index < 24; index++) {
  const durationMs = index % 3 === 0 ? between(25_000, 40_000) : between(8_000, 20_000);
  await runScenario(index, durationMs);
  simulated += durationMs;
}
await writeFile(outputPath, lines.join("\n") + "\n");
const count = (prefix: string) => lines.filter(line => line.startsWith(prefix)).length;
process.stderr.write(
  `golden-service: 24 scenarios, ${Math.round(simulated / 1000)} simulated s: ${count("t ")} frames sent, ` +
    `${count("r ")} received, ${count("a ")} actions, ${count("j ")} answers, ${count("l ")} log lines\n`
);
