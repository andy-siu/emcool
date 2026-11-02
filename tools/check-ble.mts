// End-to-end check of the Bluetooth server against the phone's real protocol code: cool-eva's
// public/lib/ble-wire.js — the module the dashboard (iOS app and GitHub Pages) runs, with
// WebCrypto doing AES-GCM and HMAC and CompressionStream doing deflate-raw. It drives
// host/build/ble_server_host (the C++ server with the radio replaced by stdin/stdout) through
// the handshake, the live stream and requests, the way public/lib/ble-link.js would.
//
//   node tools/check-ble.mts <ble_server_host> <golden decode corpus> <cool-eva checkout>

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [hostBinary, goldenPath, coolEvaDirectory] = process.argv.slice(2);
if (!hostBinary || !goldenPath || !coolEvaDirectory) {
  console.error("usage: node tools/check-ble.mts <ble_server_host> <golden decode corpus> <cool-eva checkout>");
  process.exit(2);
}

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
const SECRET = "correct horse battery staple";
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

  constructor() {
    this.#process = spawn(hostBinary, [SECRET, goldenPath, Buffer.from(storedReply).toString("hex")]);
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

  async hello(): Promise<void> {
    const info = await this.bike.info();
    const clientNonce = wire.randomNonce();
    this.key = await wire.deriveSessionKey(this.secret, info.serverNonce, clientNonce);
    this.nextCounter = 0;
    this.lastInbound = -1;
    const proof = await wire.helloProof(this.secret, info.serverNonce, clientNonce);
    await this.write(wire.clearEnvelope(wire.encodeMessage(MessageKind.HELLO, { version: 1, clientNonce, proof })));
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

async function request(
  phone: Phone,
  id: number,
  path: string,
  method = "GET",
  requestHeaders: Record<string, string> = { accept: "*/*" }
): Promise<{ status: number; headers: any; body: string }> {
  await phone.send(MessageKind.REQUEST, { id, method, path, headers: requestHeaders });
  let status = -1;
  let headers: any = {};
  const parts: Uint8Array[] = [];
  let ended = false;
  for (let round = 0; round < 20 && !ended; round++) {
    for (const message of await phone.collect(0)) {
      if (message.header?.id !== id) continue;
      if (message.kind === MessageKind.RESPONSE_HEAD) {
        status = message.header.status;
        headers = message.header.headers;
      } else if (message.kind === MessageKind.RESPONSE_BODY) {
        parts.push(message.body);
      } else if (message.kind === MessageKind.RESPONSE_END) {
        ended = true;
        check(message.header.error === undefined, `${path} ended with error ${message.header.error}`);
      }
    }
  }
  check(ended, `${path} never ended`);
  return { status, headers, body: Buffer.concat(parts).toString("utf-8") };
}

const bike = new Bike();
await bike.feed(120000);

// 1. Handshake: a wrong secret, the cooldown, then the right one.
await bike.connect(185);
await bike.notifications(true);
const stranger = new Phone(bike, "not the secret at all");
await stranger.hello();
let replies = await stranger.collect(0);
check(
  replies.length === 1 && replies[0].kind === MessageKind.REFUSED && replies[0].header.reason === "wrong secret",
  `a wrong secret got ${JSON.stringify(replies.map(reply => reply.header))}`
);
const phone = new Phone(bike, SECRET);
await phone.hello();
replies = await phone.collect(0);
check(replies[0]?.header?.reason === "too soon after a failed attempt", "the 2 s cooldown did not hold");
await bike.advance(2000);
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
check(snapshotChunks > 1, `the snapshot came in ${snapshotChunks} chunk(s); expected several at ~4.5 KB each`);
check(Math.max(...phone.frameSizes) <= 182, `a notification of ${Math.max(...phone.frameSizes)} bytes at MTU 185`);

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
const missing = await request(phone, 2, "/dl");
check(missing.status === 404 && missing.body.includes("isn't available"), `/dl: ${missing.status} ${missing.body}`);
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
check(replies[0]?.header?.reason === "no session — send HELLO first", "a sealed message after disconnect was not refused");

// 6. The smallest MTU: 20-byte notifications still carry a whole snapshot.
await bike.command("D");
await bike.connect(23);
await bike.notifications(true);
const smallPhone = new Phone(bike, SECRET);
await smallPhone.hello();
check((await smallPhone.collect(0))[0]?.kind === MessageKind.WELCOME, "no WELCOME at MTU 23");
const liveSmall = await bike.json("L");
await smallPhone.send(MessageKind.SUBSCRIBE, {});
sameLiveState("snapshot at MTU 23", mergeStream(await smallPhone.collect(0), "snapshot"), liveSmall);
check(Math.max(...smallPhone.frameSizes) <= 20, `a ${Math.max(...smallPhone.frameSizes)}-byte notification at MTU 23`);

const final = await bike.json("Z");
await bike.quit();
console.log(
  `check-ble: ${final.sessionsStarted} sessions, ${final.snapshots} snapshots (${snapshotChunks} chunks each), ` +
    `${final.requests} requests, ${final.framesSent} notifications through emcool's ble-wire.js — ${failures} failures`
);
process.exit(failures === 0 ? 0 : 1);
