// End-to-end check of the Bluetooth server against the phone's real protocol code: cool-eva's
// public/lib/ble-wire.js — the module the dashboard (iOS app and GitHub Pages) runs, with
// WebCrypto doing AES-GCM and HMAC and CompressionStream doing deflate-raw. It drives
// host/build/ble_server_host (the C++ server with the radio replaced by stdin/stdout) through
// the handshake, the live stream and requests, the way public/lib/ble-link.js would.
//
//   node tools/check-ble.mts <ble_server_host> <golden decode corpus> <cool-eva checkout> [<RIDES dir>]
//
// With a RIDES directory (check_ride_log --corpus's), /dl is downloaded from a copy of it and held
// to what the laptop reads off the card's own files.

import { execFile, spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { copyFile, cp, mkdtemp, readFile, realpath, rm, truncate, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, relative } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

import { importRideLog } from "./import-ride-log.mts";
import { collectFiles, readFileBlob, scanRecords } from "./lib/ride-log-reader.mts";

const execFileAsync = promisify(execFile);

const [hostBinary, goldenPath, coolEvaDirectory, ridesSource] = process.argv.slice(2);
if (!hostBinary || !goldenPath || !coolEvaDirectory) {
  console.error("usage: node tools/check-ble.mts <ble_server_host> <golden decode corpus> <cool-eva checkout> [<RIDES dir>]");
  process.exit(2);
}
// The server reads a copy, so the check can damage it.
const workDirectory = await mkdtemp(join(tmpdir(), "emcool-check-ble-"));
const ridesDirectory = join(workDirectory, "RIDES");
if (ridesSource) await cp(ridesSource, ridesDirectory, { recursive: true });

// ⚠️ The phone-side wire module is emcool's OWN copy now (dashboard/lib/ble-wire.js), not
// cool-eva's — emcool renamed the handshake salts (emcool/hello, emcool/session) and the request
// header (X-Emcool), so this check proves the C++ server handshakes with the dashboard emcool
// actually ships. The signal registry is still cool-eva's, unchanged, and stays imported from there.
const emcoolRoot = new URL("..", import.meta.url).pathname;
const wire = await import(pathToFileURL(join(emcoolRoot, "dashboard/lib/ble-wire.js")).href);
const registryModule = await import(pathToFileURL(join(coolEvaDirectory, "src/can/registry.ts")).href);
const registry = new Map<string, { unit: string; group: string; onDemand?: true }>(
  (registryModule.SIGNALS as { key: string; unit: string; group: string; onDemand?: true }[]).map(signal => [
    signal.key,
    signal,
  ])
);
const { MessageKind, Direction } = wire;
const load = (path: string) => import(pathToFileURL(join(coolEvaDirectory, path)).href);

// A mode 03 reply with 60 codes from Energica's table, for /stored-dtcs to serve; cool-eva's
// own stored-codes.ts files the same reply, for the body to be compared with.
const dtcTable = (await load("src/diagnostics/dtc-table.ts")).DTC_TABLE as { obdCode: string }[];
const storedCodes = dtcTable.filter((_, index) => index % 2 === 1).slice(0, 60).map(entry => {
  const letter = "PCBU".indexOf(entry.obdCode[0]);
  return (letter << 14) | (Number(entry.obdCode[1]) << 12) | parseInt(entry.obdCode.slice(2), 16);
});
const storedReply = Uint8Array.from([0x43, storedCodes.length, ...storedCodes.flatMap(code => [code >> 8, code & 0xff])]);
const coolEvaStoredDtcs = await (async () => {
  const obdDtc = await load("src/diagnostics/obd-dtc.ts");
  const stored = await load("src/diagnostics/stored-codes.ts");
  const log = console.log;
  console.log = () => {};
  stored.recordTroubleCodeRead(
    { outcome: "answered", mode: 3, response: obdDtc.decodeObdDtcResponse(storedReply, 3), payload: storedReply },
    "stored"
  );
  console.log = log;
  return stored.troubleCodeSnapshot();
})();
// A generated key, as protocol 2 requires: 32 bytes as 64 lower-case hex (dashboard/lib/ble-secret.js).
const SECRET = "5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5";
const PHONE_WRITE_FRAME_BYTES = 180;
const ALL_FRAMES = 0xffffffff;

let failures = 0;
function check(condition: boolean, message: string): void {
  if (!condition) {
    failures++;
    if (failures <= 30) console.error(`FAIL ${message}`);
  }
}

interface Packet {
  type: string;
  payload: Buffer;
}

// The C++ server, behind its stdin/stdout stand-in for the radio.
class Bike {
  #process: ChildProcessWithoutNullStreams;
  #buffer = Buffer.alloc(0);
  #packets: Packet[] = [];
  #waiting: (() => void) | null = null;

  constructor(secret = SECRET) {
    this.#process = spawn(hostBinary, [
      secret,
      goldenPath,
      Buffer.from(storedReply).toString("hex"),
      ...(ridesSource ? [ridesDirectory] : []),
    ]);
    this.#process.stderr.on("data", (chunk: Buffer) => process.stderr.write(chunk));
    this.#process.stdout.on("data", (chunk: Buffer) => {
      this.#buffer = Buffer.concat([this.#buffer, chunk]);
      while (this.#buffer.length >= 5 && this.#buffer.length >= 5 + this.#buffer.readUInt32BE(1)) {
        const length = this.#buffer.readUInt32BE(1);
        this.#packets.push({ type: String.fromCharCode(this.#buffer[0]), payload: this.#buffer.subarray(5, 5 + length) });
        this.#buffer = this.#buffer.subarray(5 + length);
      }
      this.#waiting?.();
    });
  }

  // Sends a command and resolves with everything it produced, up to its K.
  async command(type: string, payload: Uint8Array = new Uint8Array(0)): Promise<Packet[]> {
    const header = Buffer.alloc(5);
    header[0] = type.charCodeAt(0);
    header.writeUInt32BE(payload.length, 1);
    this.#process.stdin.write(Buffer.concat([header, payload]));
    while (true) {
      const done = this.#packets.findIndex(packet => packet.type === "K");
      if (done >= 0) {
        const produced = this.#packets.slice(0, done);
        this.#packets = this.#packets.slice(done + 1);
        return produced;
      }
      await new Promise<void>(resolve => (this.#waiting = resolve));
    }
  }

  connect(mtu: number) {
    const payload = Buffer.alloc(2);
    payload.writeUInt16BE(mtu);
    return this.command("C", payload);
  }
  notifications(on: boolean) {
    return this.command("N", Uint8Array.of(on ? 1 : 0));
  }
  feed(frames: number) {
    const payload = Buffer.alloc(4);
    payload.writeUInt32BE(frames);
    return this.command("P", payload);
  }
  advance(milliseconds: number, budget = ALL_FRAMES) {
    const payload = Buffer.alloc(8);
    payload.writeUInt32BE(milliseconds, 0);
    payload.writeUInt32BE(budget, 4);
    return this.command("T", payload);
  }
  async json(type: string): Promise<any> {
    const [packet] = await this.command(type);
    return JSON.parse(packet.payload.toString("utf-8"));
  }
  async info(): Promise<{ version: number; serverNonce: string }> {
    const [packet] = await this.command("R");
    return JSON.parse(packet.payload.toString("utf-8"));
  }
  quit() {
    return this.command("Q");
  }
  get pid(): number {
    return this.#process.pid!;
  }
}

interface Received {
  kind: number;
  header: any;
  body: Uint8Array;
  sealed: boolean;
  frameSizes: number[];
}

// The phone: what public/lib/ble-link.js does, minus Web Bluetooth.
class Phone {
  key: CryptoKey | null = null;
  nextCounter = 0;
  lastInbound = -1;
  messageId = 0;
  assembler = new wire.FrameAssembler(256 * 1024);
  frameSizes: number[] = [];

  bike: Bike;
  secret: string;

  constructor(bike: Bike, secret: string) {
    this.bike = bike;
    this.secret = secret;
  }

  async write(envelope: Uint8Array): Promise<void> {
    this.messageId = (this.messageId + 1) & 0xff;
    for (const frame of wire.splitIntoFrames(envelope, this.messageId, PHONE_WRITE_FRAME_BYTES)) {
      await this.bike.command("W", frame);
    }
  }

  clientNonce = "";

  async hello(): Promise<void> {
    const info = await this.bike.info();
    check(info.version === wire.PROTOCOL_VERSION, `info says protocol ${info.version}, the phone speaks ${wire.PROTOCOL_VERSION}`);
    const clientNonce = wire.randomNonce();
    this.clientNonce = clientNonce;
    this.key = await wire.deriveSessionKey(this.secret, info.serverNonce, clientNonce);
    this.nextCounter = 0;
    this.lastInbound = -1;
    const proof = await wire.helloProof(this.secret, info.serverNonce, clientNonce);
    await this.write(wire.clearEnvelope(wire.encodeMessage(MessageKind.HELLO, { version: wire.PROTOCOL_VERSION, clientNonce, proof })));
  }

  async sealed(kind: number, header: unknown, body?: Uint8Array): Promise<Uint8Array> {
    return wire.sealEnvelope(this.key, Direction.TO_PI, this.nextCounter++, wire.encodeMessage(kind, header, body));
  }

  async send(kind: number, header: unknown, body?: Uint8Array): Promise<void> {
    await this.write(await this.sealed(kind, header, body));
  }

  // Ticks the bike and opens every notification it sends.
  async collect(milliseconds = 0, budget = ALL_FRAMES): Promise<Received[]> {
    const received: Received[] = [];
    for (const packet of await this.bike.advance(milliseconds, budget)) {
      if (packet.type !== "F") continue;
      this.frameSizes.push(packet.payload.length);
      const { envelope, problem } = this.assembler.push(new Uint8Array(packet.payload));
      check(problem === undefined, `phone-side frame problem: ${problem}`);
      if (!envelope) continue;
      const parsed = wire.parseEnvelope(envelope);
      if (!parsed.sealed) {
        const message = wire.decodeMessage(parsed.message);
        received.push({ ...message, sealed: false, frameSizes: [] });
        continue;
      }
      check(parsed.counter > this.lastInbound, `counter ${parsed.counter} not above ${this.lastInbound}`);
      this.lastInbound = parsed.counter;
      let plaintext: Uint8Array;
      try {
        plaintext = await wire.openEnvelope(this.key, Direction.TO_PHONE, parsed.counter, parsed.ciphertext);
      } catch (error) {
        check(false, `the phone could not open envelope ${parsed.counter}: ${(error as Error).message}`);
        continue;
      }
      received.push({ ...wire.decodeMessage(plaintext), sealed: true, frameSizes: [] });
    }
    return received;
  }
}

function sameLiveState(description: string, signals: Record<string, any>, live: Record<string, [number, number]>) {
  const expectedKeys = Object.keys(live).sort();
  const gotKeys = Object.keys(signals).sort();
  check(
    JSON.stringify(gotKeys) === JSON.stringify(expectedKeys),
    `${description}: ${gotKeys.length} signals, the store holds ${expectedKeys.length}`
  );
  for (const [key, [value, ts]] of Object.entries(live)) {
    const reading = signals[key];
    const definition = registry.get(key);
    if (!reading) continue;
    check(Object.is(reading.value, value), `${description}: ${key} value ${reading.value}, store ${value}`);
    check(reading.ts === ts, `${description}: ${key} ts ${reading.ts}, store ${ts}`);
    check(reading.unit === definition?.unit && reading.group === definition?.group, `${description}: ${key} unit/group`);
  }
}

function mergeStream(messages: Received[], type: string): Record<string, any> {
  const merged: Record<string, any> = {};
  for (const message of messages) {
    if (message.kind === MessageKind.STREAM && message.header.type === type) {
      Object.assign(merged, message.header.signals);
    }
  }
  return merged;
}

interface Answer {
  status: number;
  headers: any;
  parts: Uint8Array[];
  bytes: Buffer;
  error: string | undefined;
  ended: boolean;
  // Stream messages that arrived between the head and the end, and the longest stretch of rounds
  // without one while the body was going.
  streamDuring: number;
  longestStreamGap: number;
  sinceStream: number;
}

interface GatherOptions {
  rounds?: number;
  milliseconds?: number;
  budget?: number;
  // Stop once this much body has come (the response still running).
  untilBytes?: number;
}

// Gathers more of response `id`: up to `rounds` × collect(milliseconds, budget).
async function gather(
  phone: Phone,
  id: number,
  answer: Answer,
  { rounds = 20, milliseconds = 0, budget = ALL_FRAMES, untilBytes = Infinity }: GatherOptions = {}
): Promise<Answer> {
  let received = answer.parts.reduce((total, part) => total + part.length, 0);
  for (let round = 0; round < rounds && !answer.ended && received < untilBytes; round++) {
    let streamed = false;
    for (const message of await phone.collect(milliseconds, budget)) {
      if (message.kind === MessageKind.STREAM && answer.status !== -1) {
        answer.streamDuring++;
        streamed = true;
      }
      if (message.header?.id !== id) continue;
      if (message.kind === MessageKind.RESPONSE_HEAD) {
        answer.status = message.header.status;
        answer.headers = message.header.headers;
      } else if (message.kind === MessageKind.RESPONSE_BODY) {
        answer.parts.push(message.body);
        received += message.body.length;
      } else if (message.kind === MessageKind.RESPONSE_END) {
        answer.ended = true;
        answer.error = message.header.error;
      }
    }
    if (answer.status !== -1 && !answer.ended) {
      answer.sinceStream = streamed ? 0 : answer.sinceStream + 1;
      answer.longestStreamGap = Math.max(answer.longestStreamGap, answer.sinceStream);
    }
  }
  answer.bytes = Buffer.concat(answer.parts);
  return answer;
}

async function exchange(
  phone: Phone,
  id: number,
  path: string,
  { method = "GET", headers = { accept: "*/*" } as Record<string, string>, body, ...options }: GatherOptions & {
    method?: string;
    headers?: Record<string, string>;
    body?: string;
  } = {}
): Promise<Answer> {
  // The body as ble-link.js sends one: the message's bytes after its header.
  await phone.send(MessageKind.REQUEST, { id, method, path, headers }, body === undefined ? undefined : new TextEncoder().encode(body));
  const answer: Answer = {
    status: -1,
    headers: {},
    parts: [],
    bytes: Buffer.alloc(0),
    error: undefined,
    ended: false,
    streamDuring: 0,
    longestStreamGap: 0,
    sinceStream: 0,
  };
  return gather(phone, id, answer, options);
}

async function request(
  phone: Phone,
  id: number,
  path: string,
  method = "GET",
  requestHeaders: Record<string, string> = { accept: "*/*" }
): Promise<{ status: number; headers: any; body: string }> {
  const answer = await exchange(phone, id, path, { method, headers: requestHeaders });
  check(answer.ended, `${path} never ended`);
  check(answer.error === undefined, `${path} ended with error ${answer.error}`);
  return { status: answer.status, headers: answer.headers, body: answer.bytes.toString("utf-8") };
}

// What /dl should send for the newest parts under `maxBytes`: each part up to its last record's end,
// oldest first — worked out from the card's files with the laptop's own reader.
async function expectedDownload(maxBytes: number): Promise<{ body: Buffer; files: string[] }> {
  const chosen: { path: string; used: Buffer }[] = [];
  let total = 0;
  for (const path of (await collectFiles([ridesDirectory])).reverse()) {
    const blob = await readFile(path);
    const records = scanRecords(blob).records;
    if (records.length === 0) continue;
    const last = records[records.length - 1];
    const used = blob.subarray(0, last.offset + last.length);
    if (chosen.length > 0 && total + used.length > maxBytes) break;
    chosen.push({ path, used });
    total += used.length;
  }
  chosen.reverse();
  return { body: Buffer.concat(chosen.map(part => part.used)), files: chosen.map(part => part.path) };
}

const bike = new Bike();
await bike.feed(120000);

// 1. Handshake: a wrong key is refused to its own nonce, with no lockout after it; the right one
// goes straight in.
await bike.connect(185);
await bike.notifications(true);
const stranger = new Phone(bike, "f".repeat(64));
await stranger.hello();
let replies = await stranger.collect(0);
check(
  replies.length === 1 &&
    replies[0].kind === MessageKind.REFUSED &&
    replies[0].header.reason === "wrong secret" &&
    replies[0].header.clientNonce === stranger.clientNonce,
  `a wrong key got ${JSON.stringify(replies.map(reply => reply.header))}`
);
// A burst of wrong HELLOs gets at most four refusals per 2 s, then silence until the window turns.
let refusals = 0;
for (let attempt = 0; attempt < 8; attempt++) {
  await stranger.hello();
  refusals += (await stranger.collect(0)).filter(reply => reply.kind === MessageKind.REFUSED).length;
}
check(refusals === 3, `${refusals} refusals in the rest of the window; four per 2 s allows 3 more`);
await bike.advance(2000);
await stranger.hello();
check((await stranger.collect(0)).length === 1, "no refusal once the 2 s window had turned");
// Frames that are not a whole HELLO are dropped unanswered before authentication.
for (const frame of [Uint8Array.of(1, 1, 0, 1), Uint8Array.of(2, 1, 9), Uint8Array.of(3, 1, 1, 0, 0)]) {
  await bike.command("W", frame);
}
check((await stranger.collect(0)).length === 0, "an unauthenticated non-HELLO frame was answered");
const phone = new Phone(bike, SECRET);
const nonceBefore = (await bike.info()).serverNonce;
await phone.hello();
replies = await phone.collect(0);
check(
  replies.length === 1 && replies[0].sealed && replies[0].kind === MessageKind.WELCOME,
  "the right secret did not get a sealed WELCOME the phone could open"
);
check((await bike.info()).serverNonce !== nonceBefore, "the server nonce did not rotate after a successful HELLO");

// 2. The live stream: the first snapshot is the whole store, exactly.
const liveBefore = await bike.json("L");
await phone.send(MessageKind.SUBSCRIBE, {});
let stream = await phone.collect(0);
const snapshot = mergeStream(stream, "snapshot");
const snapshotChunks = stream.filter(message => message.header?.type === "snapshot").length;
sameLiveState("first snapshot", snapshot, liveBefore);
check(snapshotChunks > 1, `the snapshot came in ${snapshotChunks} chunk(s); expected several at ~2.5 KB each`);
check(Math.max(...phone.frameSizes) <= 180, `a notification of ${Math.max(...phone.frameSizes)} bytes at MTU 185 (MTU − 5)`);

// Patches carry the latest values of what changed; a heartbeat snapshot then matches the store.
await bike.feed(3000);
const liveAfterFeed = await bike.json("L");
stream = await phone.collect(150);
const patch = mergeStream(stream, "patch");
check(Object.keys(patch).length > 0, "no patch after 3000 more frames");
for (const [key, reading] of Object.entries(patch)) {
  check(
    Object.is(reading.value, liveAfterFeed[key]?.[0]) && reading.ts === liveAfterFeed[key]?.[1],
    `patch ${key} = ${reading.value}, store ${liveAfterFeed[key]}`
  );
}
stream = await phone.collect(5000);
sameLiveState("heartbeat snapshot", mergeStream(stream, "snapshot"), liveAfterFeed);

// A link that drains nothing for a second coalesces changes; the next patch has the latest.
await bike.feed(3000);
await phone.collect(1000, 0);
const liveAfterStall = await bike.json("L");
stream = await phone.collect(300);
const stalledPatch = mergeStream(stream, "patch");
for (const [key, reading] of Object.entries(stalledPatch)) {
  check(Object.is(reading.value, liveAfterStall[key]?.[0]), `after the stall, patch ${key} is not the latest value`);
}

// 3. Requests: /status in cool-eva's shape; /dtc-table, /fault-infokeys and /stored-dtcs with
// cool-eva's bodies, several 4 KB pieces each; everything else a 404.
const status = await request(phone, 1, "/status");
check(status.status === 200 && status.headers["content-type"]?.startsWith("application/json"), `/status ${status.status}`);
const payload = JSON.parse(status.body);
const counters = await bike.json("Z");
check(payload.uptimeSeconds === Math.floor(counters.monotonicMs / 1000), `uptimeSeconds ${payload.uptimeSeconds}`);
check(payload.log?.enabled === true && payload.log?.files === 3 && Array.isArray(payload.waypointEvents), "/status log");
check(payload.disk?.freeBytes > 0 && payload.disk?.totalBytes > payload.disk?.freeBytes, "/status disk");
const batteryTotal = [...registry.values()].filter(signal => signal.group === "battery").length;
check(payload.groups?.battery?.[1] === batteryTotal && payload.groups.battery[0] > 0, `/status battery ${payload.groups?.battery}`);
check(payload.groups?.waypoint === undefined, "/status counted the on-demand-only waypoint group");
const missing = await request(phone, 2, "/fan");
check(missing.status === 404 && missing.body.includes("isn't available"), `/fan: ${missing.status} ${missing.body}`);
// /settings: cool-eva's SettingsReply, read-only, with every field the dashboard's views read.
const settingsReply = await request(phone, 3, "/settings");
check(settingsReply.status === 200, `/settings ${settingsReply.status}`);
const settings = JSON.parse(settingsReply.body);
for (const field of ["ok", "source", "version", "processGeneration", "instanceId", "revision", "saved", "active",
  "pending", "fault", "notices", "readiness", "restart", "locations"]) {
  check(field in settings, `/settings has no ${field}`);
}
check(
  settings.ok === true && settings.fault === null && settings.pending.length === 0 && settings.restart.state === "idle",
  `/settings state: ${JSON.stringify({ ok: settings.ok, fault: settings.fault, pending: settings.pending })}`
);
check(
  settings.active.customBmsConfigurationFlashed === true &&
    settings.active.coolantProbesEnabled === false &&
    settings.active.radiatorFanEnabled === false &&
    settings.active.phoneHotspotSsid === 'garage "wifi"' &&
    JSON.stringify(settings.saved) === JSON.stringify(settings.active),
  `/settings values: ${JSON.stringify(settings.active)}`
);
check(/^[0-9a-f-]{36}$/.test(settings.instanceId), `/settings instanceId ${settings.instanceId}`);
check(/^[0-9a-f-]{36}$/.test(settings.active.bluetoothCodeId), `/settings bluetoothCodeId ${settings.active.bluetoothCodeId}`);
check(settings.readiness.can.includes("OBD"), `/settings can readiness: ${settings.readiness.can}`);
for (const [path, method] of [["/settings", "PATCH"], ["/settings/apply", "POST"], ["/settings/hub-candidates", "POST"]]) {
  const refused = await request(phone, 4, path, method, { "X-Emcool": "settings" });
  const reply = JSON.parse(refused.body);
  check(
    refused.status === 405 && reply.ok === false && reply.message.includes("Bike console"),
    `${method} ${path}: ${refused.status} ${refused.body}`
  );
}
// Settings → Bike console: the console's commands over the link, the body carried in the REQUEST.
{
  const listed = await request(phone, 5, "/console");
  const list = JSON.parse(listed.body);
  check(listed.status === 200 && Array.isArray(list.settings) && list.revision === 0, `GET /console: ${listed.status}`);
  check(!listed.body.includes(SECRET), "GET /console sent the Bluetooth key back");
  const consoleHeaders = { "X-Emcool": "console", "Content-Type": "application/json" };
  const saved = await exchange(phone, 6, "/console", {
    method: "POST",
    headers: consoleHeaders,
    body: JSON.stringify({ line: 'set WIFI_SSID "garage wifi"', revision: list.revision }),
  });
  const reply = JSON.parse(saved.bytes.toString("utf-8"));
  check(
    saved.status === 200 && reply.ok === true && reply.revision === 1 && reply.output.includes("WIFI_SSID saved"),
    `POST /console set: ${saved.status} ${saved.bytes.toString("utf-8")}`
  );
  const shown = await exchange(phone, 7, "/console", { method: "POST", headers: consoleHeaders, body: '{"line":"show"}' });
  check(
    JSON.parse(shown.bytes.toString("utf-8")).output.includes("WIFI_SSID          garage wifi"),
    `POST /console show: ${shown.bytes.toString("utf-8")}`
  );
  const stale = await exchange(phone, 8, "/console", {
    method: "POST",
    headers: consoleHeaders,
    body: JSON.stringify({ line: "set OBD_ENABLED 0", revision: 0 }),
  });
  check(stale.status === 409, `POST /console at a stale revision: ${stale.status}`);
  const unheadered = await exchange(phone, 9, "/console", { method: "POST", body: '{"line":"show"}' });
  check(unheadered.status === 403, `POST /console without the header: ${unheadered.status}`);
  const oversized = await exchange(phone, 10, "/console", {
    method: "POST",
    headers: consoleHeaders,
    body: JSON.stringify({ line: `set BLE_NAME ${"x".repeat(2000)}`, revision: 1 }),
  });
  check(oversized.status === 413 && oversized.ended, `POST /console with an oversized body: ${oversized.status}`);
  const report = await request(phone, 11, "/console/status");
  check(report.status === 200 && report.body === "can: 812 frames/s\n\nwifi: off\n", `GET /console/status: ${report.body}`);
}
// cool-eva's Show key (POST /settings/bluetooth-key) reads the key back; emcool refuses, in the
// BluetoothKeyReply shape the dashboard reads, and the key appears nowhere in the reply.
{
  const refused = await request(phone, 4, "/settings/bluetooth-key", "POST", { "X-Emcool": "settings" });
  const reply = JSON.parse(refused.body);
  check(
    refused.status === 405 && reply.ok === false && reply.saved === null && reply.active === null &&
      reply.message.includes("set a new BLE_SECRET") && !refused.body.includes(SECRET),
    `POST /settings/bluetooth-key: ${refused.status} ${refused.body}`
  );
}
if (ridesSource) {
  await checkDownload();
} else {
  const off = await request(phone, 2, "/dl");
  check(off.status === 503 && off.body.includes("ride log is off"), `/dl with no card: ${off.status} ${off.body}`);
}

// /dl: the ride log, read off the card copy by the C++ download and carried by the server's pieces.
async function checkDownload(): Promise<void> {
  const ridesReal = await realpath(ridesDirectory);
  // The card's files the server still holds open; null where lsof isn't there to ask.
  const openUnderRides = async (): Promise<string[] | null> => {
    try {
      const { stdout } = await execFileAsync("lsof", ["-p", String(bike.pid), "-Fn"]);
      return stdout.split("\n").filter(line => line.startsWith("n" + ridesReal)).map(line => line.slice(1));
    } catch {
      return null;
    }
  };
  const nothingOpen = async (when: string) => {
    const open = await openUnderRides();
    if (open === null) return;
    check(open.length === 0, `${when}, the server still holds ${open.join(", ")}`);
  };

  // The newest megabyte. Read with the laptop's own reader it is what those files hold, and saved as
  // the phone saves it, it imports through cool-eva's db.ts.
  const wanted = await expectedDownload(1024 * 1024);
  check(wanted.files.length > 1, "the 1 MB download covers only one part; it should span several");
  const notifiedBefore = phone.frameSizes.reduce((total, size) => total + size, 0);
  const answer = await exchange(phone, 50, "/dl?mb=1", { rounds: 400 });
  const overTheAir = phone.frameSizes.reduce((total, size) => total + size, 0) - notifiedBefore;
  console.log(
    `check-ble: /dl?mb=1 sent ${answer.bytes.length} bytes of ride log in ${overTheAir} bytes of notifications ` +
      `(${(answer.bytes.length / overTheAir).toFixed(2)}× smaller over the air)`
  );
  check(answer.ended && answer.error === undefined && answer.status === 200, `/dl?mb=1: ${answer.status} ${answer.error}`);
  check(answer.headers["content-type"] === "application/octet-stream", `/dl type ${answer.headers["content-type"]}`);
  check(
    Number(answer.headers["content-length"]) === wanted.body.length,
    `/dl content-length ${answer.headers["content-length"]}, expected ${wanted.body.length}`
  );
  check(
    answer.bytes.equals(wanted.body),
    `/dl?mb=1 sent ${answer.bytes.length} bytes, expected ${wanted.body.length} from ${wanted.files.length} parts, or different`
  );
  const name = /filename="([^"]+)"/.exec(answer.headers["content-disposition"] ?? "")?.[1];
  const label = (path: string) => `${path.split("/").at(-2)}.${Number(path.split("/").at(-1)!.slice(4, 8))}`;
  check(name === `emcool-${label(wanted.files[0])}-${label(wanted.files.at(-1)!)}.log`, `/dl named ${name}`);
  const fromFiles: unknown[] = [];
  const seen = new Set<string>();
  for (const path of wanted.files) fromFiles.push(...readFileBlob(path, await readFile(path), seen).readings);
  const fromDownload = readFileBlob("dl", answer.bytes);
  check(
    fromDownload.damagedRecords === 0 && fromDownload.unrecognizedSectors === 0,
    `/dl reads with ${fromDownload.damagedRecords} damaged records, ${fromDownload.unrecognizedSectors} stray sectors`
  );
  check(
    fromDownload.readings.length > 0 && JSON.stringify(fromDownload.readings) === JSON.stringify(fromFiles),
    `/dl holds ${fromDownload.readings.length} readings, the card's parts ${fromFiles.length}`
  );
  const saved = join(workDirectory, name ?? "emcool.log");
  await writeFile(saved, answer.bytes);
  const summary = await importRideLog([saved], join(workDirectory, "rides.db"), coolEvaDirectory, () => {});
  check(
    summary.readings === fromFiles.length && summary.damagedRecords === 0,
    `/dl imported ${summary.readings} readings, expected ${fromFiles.length}`
  );
  await nothingOpen("after a finished download");

  // One boot, and what is refused.
  const four = await expectedDownload(4 * 1024 * 1024);
  const boot = await exchange(phone, 51, "/dl?boot=1&mb=4", { rounds: 400 });
  check(boot.ended && boot.status === 200 && boot.bytes.equals(four.body), `/dl?boot=1&mb=4 sent ${boot.bytes.length} bytes, expected ${four.body.length}`);
  const noBoot = await request(phone, 52, "/dl?boot=9");
  check(noBoot.status === 404 && noBoot.body.includes("no boot 9"), `/dl?boot=9: ${noBoot.status} ${noBoot.body}`);
  for (const bad of ["/dl?mb=0", "/dl?mb=257", "/dl?mb=x", "/dl?boot=-1"]) {
    const refused = await request(phone, 53, bad);
    check(refused.status === 400, `${bad}: ${refused.status} ${refused.body}`);
  }
  const posted = await request(phone, 54, "/dl", "POST");
  check(posted.status === 405, `POST /dl: ${posted.status}`);

  // At the radio's pace the live stream keeps coming while the body goes. One notification per 10 ms
  // (~18 kB/s) makes this download longer than the 5 s heartbeat; the dashboard calls a link dead after
  // 12 s with no stream message, so the heartbeats must not wait for the body.
  const paced = await exchange(phone, 55, "/dl?mb=1", { rounds: 1200, milliseconds: 100, budget: 1 });
  check(paced.ended && paced.error === undefined && paced.bytes.equals(wanted.body), `paced /dl: ${paced.bytes.length} bytes, ${paced.error}`);
  check(
    paced.streamDuring >= 2 && paced.longestStreamGap <= 60,
    `paced /dl: ${paced.streamDuring} stream messages during the body, longest gap ${paced.longestStreamGap * 100} ms`
  );

  // A part cut short under a running download: it ends with an error, never a short file passed off
  // as whole, and lets go of the file.
  const victim = wanted.files.at(-1)!;
  const cut = await exchange(phone, 56, "/dl?mb=1", { rounds: 2000, milliseconds: 10, budget: 1, untilBytes: 4096 });
  check(cut.status === 200 && !cut.ended, `the download to cut short: ${cut.status}, ended ${cut.ended}`);
  await truncate(victim, 512);
  await gather(phone, 56, cut, { rounds: 400 });
  check(
    cut.ended && cut.error !== undefined && cut.bytes.length < Number(cut.headers["content-length"]),
    `a truncated part: ended ${cut.ended}, error ${cut.error}, ${cut.bytes.length} of ${cut.headers["content-length"]} bytes`
  );
  await copyFile(join(ridesSource!, relative(ridesDirectory, victim)), victim);
  await nothingOpen("after a download that failed");

  // Gone mid-download, the phone's session takes the download with it: the file is let go, and the
  // next session downloads afresh.
  const partial = await exchange(phone, 59, "/dl?mb=1", { rounds: 2000, milliseconds: 10, budget: 1, untilBytes: 4096 });
  check(partial.status === 200 && !partial.ended, `the download to abandon: ${partial.status}, ended ${partial.ended}`);
  await bike.command("D");
  await nothingOpen("after the phone went mid-download");
  await bike.connect(185);
  await bike.notifications(true);
  await phone.hello();
  check((await phone.collect(0))[0]?.kind === MessageKind.WELCOME, "no WELCOME after the abandoned download");
  await phone.send(MessageKind.SUBSCRIBE, {});
  await phone.collect(0);
  const again = await exchange(phone, 60, "/dl?mb=1", { rounds: 400 });
  check(again.ended && again.error === undefined && again.bytes.equals(wanted.body), `/dl after an abandoned one: ${again.status} ${again.error}`);
}

for (const [id, path, module] of [
  [20, "/dtc-table", "src/http/dtc-table.ts"],
  [21, "/fault-infokeys", "src/http/fault-infokeys.ts"],
] as const) {
  const served = await request(phone, id, path);
  const expected = JSON.stringify((await load(module)).buildPayload());
  check(served.status === 200 && served.headers["content-type"]?.startsWith("application/json"), `${path} ${served.status}`);
  check(Number(served.headers["content-length"]) === Buffer.byteLength(expected), `${path} content-length`);
  check(served.body === expected, `${path}: ${served.body.length} characters, cool-eva ${expected.length}, or different`);
}
// Service mode: the snapshots straight off the card (none yet), and the rest answered later by the
// task that owns the bus — here a stand-in that answers on its third look with what it was asked.
const params = await request(phone, 30, "/vcu-params");
const coolEvaParams = await (await load("src/http/vcu-params.ts")).loadLatestSnapshot("/vcu-params");
check(
  params.status === 200 && params.body === JSON.stringify(coolEvaParams),
  `/vcu-params: ${params.status} ${params.body}, cool-eva ${JSON.stringify(coolEvaParams)}`
);
const backup = await request(phone, 31, "/vcu-backup");
check(backup.status === 404 && backup.body.startsWith("no parameter snapshot in /vcu-params yet"), `/vcu-backup: ${backup.status} ${backup.body}`);
const readState = await request(phone, 32, "/vcu-read");
check(readState.status === 202 && JSON.parse(readState.body).kind === 0, `GET /vcu-read: ${readState.status} ${readState.body}`);
const unheaded = await request(phone, 33, "/vcu-read", "POST");
check(unheaded.status === 403 && unheaded.body.includes("x-emcool: service-mode"), `POST /vcu-read without the header: ${unheaded.status}`);
const started = await request(phone, 34, "/vcu-read", "POST", { "X-Emcool": "service-mode" });
check(started.status === 202 && JSON.parse(started.body).kind === 1, `POST /vcu-read: ${started.status} ${started.body}`);
const probed = await request(phone, 35, "/vcu-probe?target=A9&bank=2&index=0x1%41", "POST", { "X-Emcool": "service-mode" });
const probe = JSON.parse(probed.body || "{}");
check(probed.status === 202 && probe.kind === 3 && probe.target === "A9" && probe.bank === "2" && probe.index === "0x1A", `POST /vcu-probe: ${probed.status} ${probed.body}`);
const wrongMethod = await request(phone, 36, "/vcu-probe?target=A9", "GET");
check(wrongMethod.status === 405, `GET /vcu-probe: ${wrongMethod.status}`);
const lifetimeStats = await request(phone, 37, "/lifetime-stats");
check(lifetimeStats.status === 202 && JSON.parse(lifetimeStats.body).kind === 6, `GET /lifetime-stats: ${lifetimeStats.status}`);
const unheadedFaults = await request(phone, 38, "/expected-faults?component=4&symptom=2&expected=1", "POST");
check(unheadedFaults.status === 403 && unheadedFaults.body.includes("expected-faults header"), `POST /expected-faults: ${unheadedFaults.status}`);
const lifetimeRead = await request(phone, 39, "/lifetime-read", "POST", { "X-Emcool": "service-mode" });
check(lifetimeRead.status === 202 && JSON.parse(lifetimeRead.body).kind === 4, `POST /lifetime-read: ${lifetimeRead.status}`);

// /vcu-write: GET (the status, no header) reaches the desk; POST needs the SEPARATE service-write
// header, deliberately not the read path's service-mode one. The stub desk echoes the kind.
const writeStatus = await request(phone, 40, "/vcu-write?list=0", "GET");
check(writeStatus.status === 202 && JSON.parse(writeStatus.body).kind === 10, `GET /vcu-write: ${writeStatus.status} ${writeStatus.body}`);
const writeUnheaded = await request(phone, 41, "/vcu-write?action=reset-vcu&confirm=reset-vcu", "POST");
check(writeUnheaded.status === 403 && writeUnheaded.body.includes("x-emcool: service-write"), `POST /vcu-write without the header: ${writeUnheaded.status} ${writeUnheaded.body}`);
const writeWrongHeader = await request(phone, 42, "/vcu-write?action=reset-vcu&confirm=reset-vcu", "POST", { "X-Emcool": "service-mode" });
check(writeWrongHeader.status === 403, `POST /vcu-write with the READ header must still be refused: ${writeWrongHeader.status}`);
const writePerform = await request(phone, 43, "/vcu-write?action=read-service-stamp", "POST", { "X-Emcool": "service-write" });
check(writePerform.status === 202 && JSON.parse(writePerform.body).kind === 11, `POST /vcu-write: ${writePerform.status} ${writePerform.body}`);
const chargeCurrent = await request(phone, 44, "/vcu-write?action=charge-current&amps=50&confirm=charge-current-50", "POST", { "X-Emcool": "service-write" });
check(chargeCurrent.status === 202 && JSON.parse(chargeCurrent.body).kind === 11, `POST /vcu-write charge-current: ${chargeCurrent.status} ${chargeCurrent.body}`);

// /charge-auto: GET the controller (no header); POST ?mode= behind the charge-auto header.
const autoStatus = await request(phone, 45, "/charge-auto", "GET");
check(autoStatus.status === 202 && JSON.parse(autoStatus.body).kind === 12, `GET /charge-auto: ${autoStatus.status} ${autoStatus.body}`);
const autoUnheaded = await request(phone, 46, "/charge-auto?mode=automatic", "POST");
check(autoUnheaded.status === 403 && autoUnheaded.body.includes("x-emcool: charge-auto"), `POST /charge-auto without the header: ${autoUnheaded.status} ${autoUnheaded.body}`);
const autoSet = await request(phone, 47, "/charge-auto?mode=automatic", "POST", { "X-Emcool": "charge-auto" });
check(autoSet.status === 202 && JSON.parse(autoSet.body).kind === 13, `POST /charge-auto: ${autoSet.status} ${autoSet.body}`);

const storedDtcs = await request(phone, 22, "/stored-dtcs");
check(storedDtcs.status === 200, `/stored-dtcs ${storedDtcs.status}`);
const servedCodes = JSON.parse(storedDtcs.body);
check(typeof servedCodes.readAt === "number" && typeof servedCodes.ageMs === "number", "/stored-dtcs has no read time");
check(Buffer.byteLength(storedDtcs.body) > 8 * 1024, `/stored-dtcs is only ${storedDtcs.body.length} characters`);
check(
  JSON.stringify({ ...servedCodes, readAt: 0, ageMs: 0 }) === JSON.stringify({ ...coolEvaStoredDtcs, readAt: 0, ageMs: 0 }),
  `/stored-dtcs differs from cool-eva's for the same reply: ${storedDtcs.body.slice(0, 300)}`
);
const noSlash = await request(phone, 3, "status");
check(noSlash.status === 400, `a path without / got ${noSlash.status}`);

// 4. Tampering and replay are dropped, and the session carries on.
const problemsBefore = (await bike.json("Z")).problems;
const genuine = await phone.sealed(MessageKind.REQUEST, { id: 4, method: "GET", path: "/status", headers: {} });
const tampered = genuine.slice();
tampered[tampered.length - 3] ^= 0x40;
await phone.write(tampered);
let afterTamper = await phone.collect(0);
check(!afterTamper.some(message => message.header?.id === 4), "a tampered request was answered");
const replayed = await phone.sealed(MessageKind.REQUEST, { id: 5, method: "GET", path: "/status", headers: {} });
await phone.write(replayed);
await phone.collect(0);
await phone.write(replayed);
afterTamper = await phone.collect(0);
check(!afterTamper.some(message => message.header?.id === 5), "a replayed request was answered twice");
check((await bike.json("Z")).problems >= problemsBefore + 2, "tampering and replay were not counted as problems");
const stillWorks = await request(phone, 6, "/status");
check(stillWorks.status === 200, "the session stopped working after tampering");

// 5. Unsubscribe stops the stream; disconnect ends the session.
await phone.send(MessageKind.UNSUBSCRIBE, {});
await phone.collect(0);
await bike.feed(2000);
stream = await phone.collect(6000);
check(!stream.some(message => message.kind === MessageKind.STREAM), "the stream kept going after UNSUBSCRIBE");
await bike.command("D");
await bike.connect(185);
await bike.notifications(true);
await phone.send(MessageKind.SUBSCRIBE, {});
replies = await phone.collect(0);
check(replies.length === 0, "a sealed message after disconnect was answered; only a HELLO gets a clear reply");

// 6. The smallest MTU: 18-byte notifications (MTU − 5) still carry a whole snapshot.
await bike.command("D");
await bike.connect(23);
await bike.notifications(true);
const smallPhone = new Phone(bike, SECRET);
await smallPhone.hello();
check((await smallPhone.collect(0))[0]?.kind === MessageKind.WELCOME, "no WELCOME at MTU 23");
const liveSmall = await bike.json("L");
await smallPhone.send(MessageKind.SUBSCRIBE, {});
sameLiveState("snapshot at MTU 23", mergeStream(await smallPhone.collect(0), "snapshot"), liveSmall);
check(Math.max(...smallPhone.frameSizes) <= 18, `a ${Math.max(...smallPhone.frameSizes)}-byte notification at MTU 23`);

// 7. A typed key (`set BLE_SECRET`, 8-64 characters with a letter and a digit): the dashboard's
// validBleSecret() takes what the firmware takes, and the key handshakes as typed, case and all.
{
  const { validBleSecret, normalizeBleSecret } = await import(
    pathToFileURL(join(emcoolRoot, "dashboard/lib/ble-secret.js")).href
  );
  for (const typed of ["bike2024", "Bike2024", SECRET.slice(0, 63), "r1de-0n!{~}", SECRET]) {
    check(validBleSecret(typed), `the dashboard refused the key ${typed}`);
  }
  for (const refused of ["correct horse battery staple", "bike 2024", "bike202", "abcdefgh", "12345678", 'bike"2024',
    "bike\\2024", "p\u00e4ss2024", "", `${SECRET}0`]) {
    check(!validBleSecret(refused), `the dashboard took the key ${refused}`);
  }
  check(normalizeBleSecret(" Bike2024 ") === "Bike2024", "a typed key lost its case on the phone");
  check(normalizeBleSecret(SECRET.toUpperCase()) === SECRET, "a generated key pasted in upper case was not lowered");
  const typedBike = new Bike("Bike2024");
  await typedBike.connect(185);
  await typedBike.notifications(true);
  const lowered = new Phone(typedBike, "bike2024");
  await lowered.hello();
  const loweredReplies = await lowered.collect(0);
  check(
    loweredReplies.length === 1 && loweredReplies[0].header.reason === "wrong secret",
    `a typed key in the wrong case got ${JSON.stringify(loweredReplies.map(reply => reply.header))}`
  );
  const typedPhone = new Phone(typedBike, "Bike2024");
  await typedPhone.hello();
  const typedReplies = await typedPhone.collect(0);
  check(
    typedReplies.length === 1 && typedReplies[0].sealed && typedReplies[0].kind === MessageKind.WELCOME,
    `a typed key got ${JSON.stringify(typedReplies.map(reply => reply.header))}`
  );
  await typedBike.quit();
}

const final = await bike.json("Z");
await bike.quit();
await rm(workDirectory, { recursive: true, force: true });
console.log(
  `check-ble: ${final.sessionsStarted} sessions, ${final.snapshots} snapshots (${snapshotChunks} chunks each), ` +
    `${final.requests} requests, ${final.framesSent} notifications through emcool's ble-wire.js — ${failures} failures`
);
process.exit(failures === 0 ? 0 : 1);
