// Drives the reference's own OBD code — src/can/obd.ts, obd-dtc.ts, obd-hold.ts, elock.ts, iso-tp.ts
// and src/diagnostics/obd-dtc.ts, stored-codes.ts, dtc-table.ts — and writes what it did, for
// host/tests/check_obd.cpp to replay through the C++ port.
//
// Two parts:
// - the pure functions, exhaustively or by fuzzing: formatObdDtc and the table lookup for all
//   65 536 codes, every NRC's description, random ISO-TP frame sequences, random replies
//   through decodeObdDtcResponse
// - scenarios: the poller (and the E-LOCK read) started as the reference's index.ts starts them,
//   against a scripted ECU on a fake clock — PIDs answered, late, short or not at all;
//   trouble-code lists as single frames, multi-frame transfers that complete, stall, skip a
//   sequence number, restart or overflow, refusals and silence; flow control the poller must
//   refuse; noise on the diagnostic ids; sends that fail; holds that park the poller, are
//   refused, released, or outstay the 15 s cap. Every frame sent, signal recorded and line
//   logged is written with its millisecond, and /stored-dtcs' body at random moments.
//
// Each scenario gets fresh module instances (a resolve hook tags the reference's imports with the
// run number), and the reference's record() is instrumented by a load hook to report each call.
//
//   node tools/generate-golden-obd.mts <reference checkout> <output file>
//
// Timing rules, which check_obd.cpp follows too: time is whole milliseconds; within one
// millisecond, scheduled actions run first, then the reference's due timers, then the frames that
// arrive in it. setTimeout keeps Node's rule that a delay under 1 ms is 1 ms.
//
// Lines (fields are space-separated; a trailing text field may contain spaces):
//   F <raw> <code> <component> <symptom>      formatObdDtc + lookupByObdCode (-1 -1 if unlisted)
//   N <nrc> <description>
//   I                                         a new ISO-TP reassembler
//   P <frame hex> <status> <total> <payload hex|-> <reason|->
//   D <mode> <payload hex|-> <kind> <list> <declared> <codes hex,…|-> <truncated> <service> <nrc> <reason|->
//   run <n> <elock 0|1> <wall clock base ms> <end ms>
//   a <ms> hold <wait ms> <max hold ms> <reason> | a <ms> release | a <ms> snapshot
//   r <ms> <id hex> <data hex>                a frame arriving
//   t <ms> <id hex> <data hex> <ok|fail>      a frame the reference sent (fail: send threw)
//   s <ms> <key> <value>                      record(key, value)
//   l <ms> <I|W|E> <text>                     a log line (its first argument)
//   j <ms> <json>                             JSON.stringify(troubleCodeSnapshot())
//   end

import { registerHooks } from "node:module";
import { writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  process.stderr.write("usage: node tools/generate-golden-obd.mts <reference checkout> <output file>\n");
  process.exit(2);
}
const srcRoot = pathToFileURL(resolve(referenceDirectory, "src")).href + "/";

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
      const source = String(result.source);
      const marker = "export function record(key: string, value: number, ts: number = Date.now()): void {";
      if (!source.includes(marker)) throw new Error("the reference's record() has changed shape; update the hook");
      return { ...result, source: source.replace(marker, `${marker} (globalThis as any).__emcoolRecorded(key, value);`) };
    }
    return result;
  },
});

const importFresh = (path: string) => import(`${srcRoot}${path}?gen=${generation}`);

// ---------------------------------------------------------------------------------------------
// The fake clock and the output.

const lines: string[] = [];
let now = 0;
let wallBase = 0;
const emit = (line: string) => void lines.push(line);
const hex = (bytes: ArrayLike<number>) => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");

Object.defineProperty(performance, "now", { value: () => now, configurable: true, writable: true });
Date.now = () => wallBase + now;

// A min-heap on (at, seq): the order things were scheduled in breaks ties, as Node's timers do.
class Queue<T extends { at: number; seq: number }> {
  #items: T[] = [];
  get size(): number {
    return this.#items.length;
  }
  peek(): T | undefined {
    return this.#items[0];
  }
  push(item: T): void {
    const items = this.#items;
    items.push(item);
    for (let at = items.length - 1; at > 0; ) {
      const parent = (at - 1) >> 1;
      if (!Queue.#before(items[at], items[parent])) break;
      [items[at], items[parent]] = [items[parent], items[at]];
      at = parent;
    }
  }
  pop(): T | undefined {
    const items = this.#items;
    const top = items[0];
    const last = items.pop();
    if (items.length > 0 && last !== undefined) {
      items[0] = last;
      for (let at = 0; ; ) {
        const left = 2 * at + 1;
        const right = left + 1;
        let smallest = at;
        if (left < items.length && Queue.#before(items[left], items[smallest])) smallest = left;
        if (right < items.length && Queue.#before(items[right], items[smallest])) smallest = right;
        if (smallest === at) break;
        [items[at], items[smallest]] = [items[smallest], items[at]];
        at = smallest;
      }
    }
    return top;
  }
  static #before(left: { at: number; seq: number }, right: { at: number; seq: number }): boolean {
    return left.at < right.at || (left.at === right.at && left.seq < right.seq);
  }
}

interface FakeTimer {
  at: number;
  seq: number;
  callback: () => void;
  cancelled: boolean;
}
let timers = new Queue<FakeTimer>();
let timerSeq = 0;
globalThis.setTimeout = ((callback: (...args: unknown[]) => void, delay?: number, ...args: unknown[]) => {
  let after = Number(delay);
  if (!(after >= 1 && after <= 2_147_483_647)) after = 1;
  const timer: FakeTimer & { unref(): unknown; ref(): unknown; hasRef(): boolean } = {
    at: now + after,
    seq: timerSeq++,
    callback: () => callback(...args),
    cancelled: false,
    unref: () => timer,
    ref: () => timer,
    hasRef: () => true,
  };
  timers.push(timer);
  return timer;
}) as unknown as typeof setTimeout;
// The earliest live timer, dropping cancelled ones on the way.
function nextTimer(): FakeTimer | undefined {
  while (timers.peek()?.cancelled) timers.pop();
  return timers.peek();
}
globalThis.clearTimeout = ((timer: FakeTimer | undefined) => {
  if (timer) timer.cancelled = true;
}) as typeof clearTimeout;
globalThis.setInterval = (() => {
  throw new Error("setInterval is not simulated");
}) as unknown as typeof setInterval;

const realSetImmediate = globalThis.setImmediate;
const drain = () => new Promise<void>(done => realSetImmediate(done));

(globalThis as any).__emcoolRecorded = (key: string, value: number) => emit(`s ${now} ${key} ${value}`);
const logLine = (level: string) => (...args: unknown[]) => emit(`l ${now} ${level} ${String(args[0])}`);
console.log = logLine("I");
console.info = logLine("I");
console.warn = logLine("W");
console.error = logLine("E");

// ---------------------------------------------------------------------------------------------

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
const random = mulberry32(0x7df);
const chance = (probability: number) => random() < probability;
const between = (low: number, high: number) => low + Math.floor(random() * (high - low + 1));
const pick = <T,>(items: readonly T[]): T => items[Math.floor(random() * items.length)];
const randomBytes = (count: number) => Array.from({ length: count }, () => between(0, 255));

// ---------------------------------------------------------------------------------------------
// The pure functions.

const dtcTable = await importFresh("diagnostics/dtc-table.ts");
const obdDtc = await importFresh("diagnostics/obd-dtc.ts");
const isoTp = await importFresh("can/iso-tp.ts");

const tableCodes: number[] = [];
for (let raw = 0; raw < 0x10000; raw++) {
  const code = dtcTable.formatObdDtc(raw) as string;
  const entry = dtcTable.lookupByObdCode(code);
  emit(`F ${raw} ${code} ${entry ? `${entry.component} ${entry.symptom}` : "-1 -1"}`);
  if (entry) tableCodes.push(raw);
}
for (let code = 0; code < 256; code++) emit(`N ${code} ${obdDtc.describeNegativeResponseCode(code)}`);

const pciNibbles = [0, 0, 1, 1, 2, 2, 2, 2, 3, 4, 7, 15];
function fuzzFrame(): number[] {
  const length = chance(0.8) ? 8 : between(0, 8);
  const frame = randomBytes(length);
  if (length > 0) frame[0] = (pick(pciNibbles) << 4) | (chance(0.5) ? between(0, 15) : between(0, 3));
  return frame;
}
let isoPushes = 0;
for (let sequence = 0; sequence < 3000; sequence++) {
  emit("I");
  const reassembler = new isoTp.IsoTpReassembler();
  const pushes = between(1, 40);
  let nextSequence = 1;
  for (let index = 0; index < pushes; index++) {
    let frame: number[];
    const shape = random();
    if (shape < 0.15) {
      // A plausible first frame, sometimes over the cap.
      const total = chance(0.1) ? between(1000, 4095) : between(0, 300);
      frame = [0x10 | (total >> 8), total & 0xff, ...randomBytes(6)];
      nextSequence = 1;
    } else if (shape < 0.75) {
      // A consecutive frame, usually the next in sequence.
      frame = [0x20 | (chance(0.95) ? nextSequence : between(0, 15)), ...randomBytes(chance(0.8) ? 7 : between(0, 7))];
      nextSequence = (nextSequence + 1) & 0x0f;
    } else {
      frame = fuzzFrame();
    }
    const result = reassembler.push(Uint8Array.from(frame));
    isoPushes++;
    emit(
      `P ${hex(frame) || "-"} ${result.status} ${result.status === "flow-control-required" ? result.totalLength : 0} ` +
        `${result.status === "complete" ? hex(result.payload) || "-" : "-"} ${result.reason ?? "-"}`
    );
  }
  // Long runs of frames that are ignored but still counted, which is how the cap is reached.
  if (chance(0.03)) {
    for (let index = 0; index < 150; index++) {
      const frame = pick([[0x30, 0, 0], [0x40 | between(0, 15)], [0x00, 1], [0xf0, ...randomBytes(7)]]);
      const result = reassembler.push(Uint8Array.from(frame));
      isoPushes++;
      emit(
        `P ${hex(frame)} ${result.status} ${result.status === "flow-control-required" ? result.totalLength : 0} ` +
          `${result.status === "complete" ? hex(result.payload) || "-" : "-"} ${result.reason ?? "-"}`
      );
    }
  }
  // A long, valid transfer now and then, to reach the frame cap.
  if (chance(0.05)) {
    const total = between(900, 1024);
    const frames = [[0x10 | (total >> 8), total & 0xff, ...randomBytes(6)]];
    for (let index = 1; index < 160; index++) frames.push([0x20 | (index & 0x0f), ...randomBytes(7)]);
    for (const frame of frames) {
      const result = reassembler.push(Uint8Array.from(frame));
      emit(
        `P ${hex(frame)} ${result.status} ${result.status === "flow-control-required" ? result.totalLength : 0} ` +
          `${result.status === "complete" ? hex(result.payload) || "-" : "-"} ${result.reason ?? "-"}`
      );
    }
  }
}

const modes = [0x03, 0x07, 0x0a, 0x01, 0x04];
for (let index = 0; index < 4000; index++) {
  const mode = chance(0.9) ? pick([0x03, 0x07, 0x0a]) : pick(modes);
  let payload: number[];
  const shape = random();
  if (shape < 0.6) {
    const codes = Array.from({ length: between(0, 20) }, () =>
      chance(0.6) ? pick(tableCodes) : chance(0.2) ? 0 : between(1, 0xffff)
    );
    const declared = chance(0.7) ? codes.filter(code => code !== 0).length : between(0, 30);
    payload = [chance(0.9) ? mode + 0x40 : between(0, 255), declared];
    for (const code of codes) payload.push(code >> 8, code & 0xff);
    if (chance(0.1)) payload.push(between(0, 255));
  } else if (shape < 0.8) {
    payload = [0x7f, ...randomBytes(between(0, 2))];
  } else {
    payload = randomBytes(between(0, 6));
  }
  const response = obdDtc.decodeObdDtcResponse(Uint8Array.from(payload), mode);
  const fields =
    response.kind === "codes"
      ? `codes ${response.list} ${response.declaredCount} ${
          response.codes.map((code: { raw: number }) => code.raw.toString(16)).join(",") || "-"
        } ${response.truncated ? 1 : 0} 0 0 -`
      : response.kind === "negative"
        ? `negative - 0 - 0 ${response.requestedService} ${response.negativeResponseCode} -`
        : `unrecognised - 0 - 0 0 0 ${response.reason}`;
  emit(`D ${mode} ${hex(payload) || "-"} ${fields}`);
}

// ---------------------------------------------------------------------------------------------
// The scenarios: a scripted ECU behind a fake channel.

interface Arrival {
  at: number;
  seq: number;
  id: number;
  data: number[];
}

type DtcBehaviour =
  | "complete"
  | "silent"
  | "negative"
  | "stall"
  | "skip-sequence"
  | "restart"
  | "oversize"
  | "wrong-service"
  | "late"
  | "no-flow-control-wait";

interface Profile {
  pidAnswer: number;
  pidLate: number;
  pidShort: number;
  secondEcu: number;
  responder: number;
  noisePerSecond: number;
  sendFail: number;
  stored: [DtcBehaviour, number][];
  silentModes: [DtcBehaviour, number][];
  codes: number[];
  declaredDelta: number;
  separationMs: number;
  elock: string;
  elockLate: number;
}

function weighted<T>(choices: [T, number][]): T {
  const total = choices.reduce((sum, [, weight]) => sum + weight, 0);
  let roll = random() * total;
  for (const [choice, weight] of choices) {
    roll -= weight;
    if (roll < 0) return choice;
  }
  return choices[choices.length - 1][0];
}

function makeProfile(): Profile {
  const quiet = chance(0.15);
  return {
    pidAnswer: quiet ? 0 : pick([1, 1, 0.97, 0.8, 0.5]),
    pidLate: pick([0, 0, 0.02, 0.1]),
    pidShort: pick([0, 0.05, 0.3]),
    secondEcu: pick([0, 0, 0.1, 0.5]),
    responder: chance(0.85) ? 0x7e8 : pick([0x7e9, 0x7ea, 0x7e0, 0x7e3]),
    noisePerSecond: pick([0, 0, 1, 5, 20]),
    sendFail: pick([0, 0, 0, 0.01, 0.1]),
    stored: quiet
      ? [["silent", 1]]
      : [
          ["complete", pick([10, 3, 1])],
          ["silent", pick([0, 1, 3])],
          ["negative", pick([0, 1])],
          ["stall", pick([0, 1, 3, 40])],
          ["skip-sequence", pick([0, 1])],
          ["restart", pick([0, 1])],
          ["oversize", pick([0, 0.3])],
          ["wrong-service", pick([0, 0.5])],
          ["late", pick([0, 0.5, 1])],
          ["no-flow-control-wait", pick([0, 0.5])],
        ],
    silentModes: [
      ["silent", 8],
      ["negative", pick([0, 1])],
      ["complete", pick([0, 1])],
      ["stall", pick([0, 0.5])],
    ],
    codes: Array.from({ length: pick([0, 1, 2, 3, 5, 39, 39, 60]) }, () =>
      chance(0.85) ? pick(tableCodes) : between(1, 0xffff)
    ),
    declaredDelta: chance(0.8) ? 0 : pick([-2, 1, 3]),
    separationMs: pick([0, 1, 2, 5]),
    elock: pick(["direct", "session", "silent", "negative", "garbage", "session-refused", "session-no-keys", "short"]),
    elockLate: pick([0, 0, 0.3]),
  };
}

// A scenario pinned down instead of drawn: a profile to override, and the holds to ask for.
interface Fixed {
  profile: Partial<Profile>;
  holds: { at: number; waitMs: number; maxHoldMs: number; releaseAfter: number | null }[];
}

async function runScenario(index: number, durationMs: number, elockEnabled: boolean, fixed?: Fixed): Promise<void> {
  generation = index + 1;
  timers = new Queue<FakeTimer>();
  timerSeq = 0;
  now = 0;
  wallBase = 1_780_000_000_000 + index * 3_600_000;
  const obd = await importFresh("can/obd.ts");
  const elock = await importFresh("can/elock.ts");
  const stored = await importFresh("diagnostics/stored-codes.ts");
  const profile = { ...makeProfile(), ...fixed?.profile };
  emit(`run ${index} ${elockEnabled ? 1 : 0} ${wallBase} ${durationMs}`);

  const arrivals = new Queue<Arrival>();
  let arrivalSeq = 0;
  const arrive = (at: number, id: number, data: number[]) => arrivals.push({ at, seq: arrivalSeq++, id, data });

  // The ECU's half of a multi-frame transfer that's waiting for flow control.
  let transfer: { responder: number; frames: number[][]; behaviour: DtcBehaviour } | null = null;
  let elockSession = false;

  function replyFrames(mode: number, behaviour: DtcBehaviour): number[][] {
    const codes = mode === 0x03 ? profile.codes : profile.codes.slice(0, between(0, 3));
    const declared = Math.max(0, codes.length + profile.declaredDelta);
    const service = behaviour === "wrong-service" ? 0x41 : mode + 0x40;
    const payload = [service, declared];
    for (const code of codes) payload.push(code >> 8, code & 0xff);
    if (payload.length <= 7) {
      const frame = [payload.length, ...payload];
      while (frame.length < 8) frame.push(chance(0.5) ? 0x00 : 0x55);
      return [frame];
    }
    const total = behaviour === "oversize" ? between(1025, 4095) : payload.length;
    const frames = [[0x10 | (total >> 8), total & 0xff, ...payload.slice(0, 6)]];
    let sequence = 1;
    for (let offset = 6; offset < payload.length; offset += 7) {
      const frame = [0x20 | sequence, ...payload.slice(offset, offset + 7)];
      while (frame.length < 8 && chance(0.9)) frame.push(0x00);
      frames.push(frame);
      sequence = (sequence + 1) & 0x0f;
    }
    return frames;
  }

  function answerTroubleCodes(mode: number): void {
    const behaviour = weighted(mode === 0x03 ? profile.stored : profile.silentModes);
    const delay = between(1, 40);
    if (behaviour === "silent") return;
    if (behaviour === "negative") {
      arrive(now + delay, profile.responder, [0x03, 0x7f, mode, pick([0x11, 0x12, 0x22, 0x31, 0x78, 0x99])]);
      return;
    }
    const frames = replyFrames(mode, behaviour);
    const at = behaviour === "late" ? now + between(301, 700) : now + delay;
    arrive(at, profile.responder, frames[0]);
    if (frames.length === 1) return;
    if (behaviour === "no-flow-control-wait") {
      // Consecutive frames without waiting to be asked.
      frames.slice(1).forEach((frame, offset) => arrive(at + 1 + offset, profile.responder, frame));
      return;
    }
    transfer = { responder: profile.responder, frames: frames.slice(1), behaviour };
  }

  function continueTransfer(): void {
    const pending = transfer;
    transfer = null;
    if (!pending) return;
    let frames = pending.frames;
    if (pending.behaviour === "stall") frames = frames.slice(0, between(0, Math.max(0, frames.length - 1)));
    if (pending.behaviour === "skip-sequence" && frames.length > 1) {
      const skipped = between(0, frames.length - 1);
      frames = frames.map((frame, at) => (at === skipped ? [(frame[0] & 0xf0) | ((frame[0] + 1) & 0x0f), ...frame.slice(1)] : frame));
    }
    let at = now;
    frames.forEach((frame, offset) => {
      at += Math.max(1, profile.separationMs);
      arrive(at, pending.responder, frame);
      if (pending.behaviour === "restart" && offset === 1) {
        // The ECU starts the whole reply again, and waits for flow control again.
        at += 1;
        arrive(at, pending.responder, replyFrames(0x03, "complete")[0]);
      }
    });
  }

  function answerElock(data: Buffer): void {
    const service = data[1];
    const delay = chance(profile.elockLate) ? between(301, 500) : between(1, 40);
    const reply = (frame: number[]) => {
      while (frame.length < 8) frame.push(0);
      arrive(now + delay, 0x790, frame);
    };
    const keys = between(0, 4);
    switch (profile.elock) {
      case "direct":
        if (service === 0x21) reply([0x03, 0x61, 0x99, keys]);
        return;
      case "session":
      case "session-no-keys":
        if (service === 0x21 && !elockSession) reply([0x03, 0x7f, 0x21, 0x12]);
        else if (service === 0x21 && profile.elock === "session") reply([0x03, 0x61, 0x99, keys]);
        else if (service === 0x10) {
          elockSession = true;
          reply([0x02, 0x50, 0x81]);
        } else if (service === 0x20) {
          elockSession = false;
          reply([0x01, 0x60]);
        }
        return;
      case "negative":
        reply([0x03, 0x7f, service, 0x11]);
        return;
      case "session-refused":
        reply(service === 0x10 ? [0x03, 0x7f, 0x10, 0x22] : [0x03, 0x7f, service, 0x12]);
        return;
      case "garbage":
        reply(randomBytes(between(0, 8)));
        return;
      case "short":
        reply(pick([[0x01, 0x61], [0x02, 0x61, 0x98], [0x00], [0x05, 0x61, 0x99]]));
        return;
      default:
        return;
    }
  }

  const channel = {
    send(message: { id: number; data: Buffer }) {
      const fail = chance(profile.sendFail);
      emit(`t ${now} ${message.id.toString(16)} ${hex(message.data)} ${fail ? "fail" : "ok"}`);
      if (fail) throw new Error("simulated send failure");
      const data = message.data;
      if (message.id === 0x7df && data[0] === 0x02 && data[1] === 0x01) {
        if (!chance(profile.pidAnswer)) return;
        const pid = data[2];
        const late = chance(profile.pidLate);
        const at = now + (late ? between(201, 320) : between(1, 30));
        const short = chance(profile.pidShort);
        const answer = [short ? 0x02 : 0x04, 0x41, pid, between(0, 255), between(0, 255)];
        arrive(at, 0x7e8, short ? answer.slice(0, between(3, 4)) : [...answer, 0x55, 0x55, 0x55]);
        if (chance(profile.secondEcu)) arrive(at + between(0, 3), 0x7e9, [0x03, 0x41, pid, between(0, 255), 0, 0, 0, 0]);
      } else if (message.id === 0x7df && data[0] === 0x01) {
        answerTroubleCodes(data[1]);
      } else if (message.id >= 0x7e0 && message.id <= 0x7e7 && data[0] === 0x30) {
        if (transfer && transfer.responder - 8 === message.id) continueTransfer();
      } else if (message.id === 0x791) {
        answerElock(data);
      }
    },
  };

  // Noise on the diagnostic ids: stray consecutive frames, other PIDs' answers, junk.
  for (let at = between(1, 3000); profile.noisePerSecond > 0 && at < durationMs; at += between(1, 2000 / profile.noisePerSecond)) {
    const id = chance(0.05) ? 0x790 : between(0x7e0, 0x7ef);
    const shape = random();
    const data =
      shape < 0.3
        ? [0x20 | between(0, 15), ...randomBytes(7)]
        : shape < 0.6
          ? [0x03, 0x41, between(0, 255), between(0, 255), 0, 0, 0, 0]
          : shape < 0.7
            ? [0x10, between(0, 80), 0x43, ...randomBytes(5)]
            : randomBytes(between(0, 8));
    arrive(at, id, data);
  }

  // Holds: asked at random, sometimes over another, released (or not) a while later.
  const actions: { at: number; snapshot: boolean; run: () => void }[] = [];
  let current: { release: () => void } | null = null;
  let holdNumber = 0;
  const holds =
    fixed?.holds ??
    (() => {
      const drawn: Fixed["holds"] = [];
      for (let at = between(1, 20000); at < durationMs; at += between(500, 40000)) {
        const maxHoldMs = pick([15000, 15000, 3000, 400]);
        drawn.push({
          at,
          waitMs: pick([6000, 6000, 300, 50]),
          maxHoldMs,
          releaseAfter: chance(0.85) ? between(1, maxHoldMs + 2000) : null,
        });
      }
      return drawn;
    })();
  for (const { at, waitMs, maxHoldMs, releaseAfter } of holds) {
    const reason = `scenario hold ${++holdNumber}`;
    actions.push({
      at,
      snapshot: false,
      run: () => {
        emit(`a ${now} hold ${waitMs} ${maxHoldMs} ${reason}`);
        void obd.holdObdPoller(reason, { waitMs, maxHoldMs }).then((hold: { release: () => void } | null) => {
          current = hold;
        });
      },
    });
    if (releaseAfter !== null) {
      actions.push({
        at: at + releaseAfter,
        snapshot: false,
        run: () => {
          if (!current) return;
          emit(`a ${now} release`);
          current.release();
          current = null;
        },
      });
    }
  }
  for (let count = between(1, 6); count > 0; count--) {
    actions.push({ at: between(1, durationMs), snapshot: true, run: () => emit(`a ${now} snapshot`) });
  }
  // Stable, so a hold and its release in the same millisecond stay in that order.
  actions.sort((left, right) => left.at - right.at);

  obd.initObd(channel);
  const stop = obd.startObdPoller(500);
  if (elockEnabled) void elock.readKeysPairedOnce(channel);
  await drain();

  let nextAction = 0;
  while (true) {
    const candidates = [nextTimer()?.at, arrivals.peek()?.at, actions[nextAction]?.at].filter(
      (candidate): candidate is number => candidate !== undefined
    );
    if (candidates.length === 0) break;
    const at = Math.min(...candidates);
    if (at > durationMs) break;
    now = at;
    while (nextAction < actions.length && actions[nextAction].at === at) {
      const action = actions[nextAction++];
      action.run();
      if (action.snapshot) emit(`j ${now} ${JSON.stringify(stored.troubleCodeSnapshot())}`);
      await drain();
    }
    for (let due = nextTimer(); due !== undefined && due.at <= at; due = nextTimer()) {
      timers.pop();
      due.callback();
      await drain();
    }
    while (arrivals.peek() !== undefined && arrivals.peek()!.at <= at) {
      const due = arrivals.pop()!;
      emit(`r ${now} ${due.id.toString(16)} ${hex(due.data) || "-"}`);
      const data = Buffer.from(due.data);
      if (obd.isObdResponse(due.id)) obd.handleResponse(due.id, data);
      else if (elock.isElockResponse(due.id)) elock.handleElockResponse(data);
      await drain();
    }
  }
  now = durationMs + 1;
  emit(`a ${now} snapshot`);
  emit(`j ${now} ${JSON.stringify(stored.troubleCodeSnapshot())}`);
  emit("end");
  stop();
  timers = new Queue<FakeTimer>();
}

const totals = { runs: 0, simulatedMs: 0 };
for (let index = 0; index < 70; index++) {
  const durationMs = index % 10 === 3 ? between(600_000, 1_300_000) : between(2_000, 200_000);
  await runScenario(index, durationMs, chance(0.7));
  totals.runs++;
  totals.simulatedMs += durationMs;
}

// A silent bike, so the poller's timeline is fixed: PIDs at 0, 200 … 1600, each timing out
// 200 ms later; the three trouble-code reads at 1800, 2100 and 2400, each silent after 300 ms;
// then, the round long overrun, a 1 ms sleep set at 2700 and the next round at 2701. Two holds
// whose wait runs out in the very millisecond the poller next looks, where Node runs whichever
// timer was set first:
// - asked at 550 for 50 ms: the poller's PID timeout for 600 was set at 400, so it looks first
//   and parks — granted
// - asked at 2500 for 201 ms: the 2701 wake-up was set at 2700, after it, so the wait runs out
//   first — refused, and the round goes ahead
const silent: Partial<Profile> = {
  pidAnswer: 0,
  noisePerSecond: 0,
  sendFail: 0,
  stored: [["silent", 1]],
  silentModes: [["silent", 1]],
};
const coincidences: Fixed["holds"][] = [
  [{ at: 550, waitMs: 50, maxHoldMs: 1000, releaseAfter: 500 }],
  [{ at: 2500, waitMs: 201, maxHoldMs: 1000, releaseAfter: 500 }],
];
for (const holds of coincidences) {
  await runScenario(totals.runs, 6000, false, { profile: silent, holds });
  totals.runs++;
  totals.simulatedMs += 6000;
}

await writeFile(outputPath, lines.join("\n") + "\n");
const count = (prefix: string) => lines.filter(line => line.startsWith(prefix)).length;
process.stderr.write(
  `golden-obd: ${isoPushes} ISO-TP pushes, ${count("D ")} replies decoded; ${totals.runs} scenarios, ` +
    `${Math.round(totals.simulatedMs / 60000)} simulated minutes: ${count("t ")} frames sent, ${count("r ")} received, ` +
    `${count("s ")} signals, ${count("l ")} log lines, ${count("j ")} snapshots\n`
);
