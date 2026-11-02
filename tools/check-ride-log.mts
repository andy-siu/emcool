// End-to-end check of the ride log, laptop side, against the files host/tests/check_ride_log
// --corpus wrote (the decoder corpus through the C++ pipeline) and the list of every reading
// it was given:
//
//   1. Read back, the files hold exactly those readings: ts, key, value, seq, session, trust.
//   2. Simulated power cuts and card damage — truncation at random points, random sectors
//      overwritten with garbage, zeros or a stale copy, a torn final record — lose only the
//      records they touch, and never produce a wrong reading.
//   3. Importing into a fresh rides.db through cool-eva's own db.ts stores every reading.
//
// With --rotated the card filled during the run and old parts were deleted, so (1) becomes:
// what remains is exactly the newest readings — a contiguous tail of the expected list,
// nothing missing in between — and (3) imports that tail.
//
//   node tools/check-ride-log.mts <corpus dir> <expected.jsonl> <cool-eva checkout> [--rotated]

import { mkdtemp, readFile, rm } from "node:fs/promises";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { importRideLog } from "./import-ride-log.mts";
import {
  HEADER_BYTES,
  SECTOR_BYTES,
  collectFiles,
  readFileBlob,
  readRideLog,
  scanRecords,
  type RideLogReading,
} from "./lib/ride-log-reader.mts";

interface ExpectedReading {
  ts: number;
  key: string;
  value: number;
  seq: number;
  trust: string;
}

const [corpusDirectory, expectedPath, coolEvaDirectory, mode] = process.argv.slice(2);
if (!corpusDirectory || !expectedPath || !coolEvaDirectory || (mode !== undefined && mode !== "--rotated")) {
  console.error("usage: node tools/check-ride-log.mts <corpus dir> <expected.jsonl> <cool-eva checkout> [--rotated]");
  process.exit(2);
}
const rotated = mode === "--rotated";

let failures = 0;
const fail = (message: string): void => {
  failures++;
  if (failures <= 20) {
    console.error(`FAIL ${message}`);
  }
};

const expectedLines = (await readFile(expectedPath, "utf-8")).split("\n").filter(line => line.length > 0);
const session = (JSON.parse(expectedLines[0]) as { session: string }).session;
const given = expectedLines.slice(1).map(line => JSON.parse(line) as ExpectedReading);

// 1. Exact read-back — of everything, or after rotation of the newest tail.
const readouts = await readRideLog([join(corpusDirectory, "RIDES")]);
const readings = readouts.flatMap(readout => readout.readings);
if (rotated && (readings.length === 0 || readings.length >= given.length)) {
  fail(`read back ${readings.length} of ${given.length} readings; a rotated card should hold a non-empty part of them`);
}
const expected = rotated ? given.slice(given.length - readings.length) : given;
if (readings.length !== expected.length) {
  fail(`read back ${readings.length} readings, the firmware was given ${expected.length}`);
}
for (let index = 0; index < Math.min(readings.length, expected.length); index++) {
  const actual = readings[index];
  const wanted = expected[index];
  if (
    actual.ts !== wanted.ts ||
    actual.key !== wanted.key ||
    !Object.is(actual.value, wanted.value) ||
    actual.seq !== wanted.seq ||
    actual.clockTrust !== wanted.trust ||
    actual.session !== session
  ) {
    fail(`reading ${index}: read ${JSON.stringify(actual)}, expected ${JSON.stringify(wanted)} in session ${session}`);
  }
}
const damaged = readouts.reduce((total, readout) => total + readout.damagedRecords, 0);
if (damaged > 0) {
  fail(`${damaged} damaged records in an undamaged corpus`);
}
for (const readout of readouts.filter(readout => readout.records.length > 0)) {
  const unlabelled = [...new Set(readout.readings.map(reading => reading.key))].filter(key => !readout.meta.has(key));
  if (unlabelled.length > 0) {
    fail(`${readout.path}: no unit/group/source in the file for ${unlabelled.join(", ")}`);
  }
}
const located = readings.filter(reading => /^(gps_(lat|lon|course_deg|altitude_m)|waypoint_l(at|on))$/.test(reading.key));
if (located.length > 0) {
  fail(`${located.length} location readings reached the card`);
}
console.log(
  rotated
    ? `check-ride-log: read back the newest ${readings.length} of ${given.length} readings from ${readouts.length} files — exact, contiguous`
    : `check-ride-log: read back ${readings.length} readings from ${readouts.length} files — exact`
);

// 2. Power cuts and damage, one file at a time.
const files = (await collectFiles([join(corpusDirectory, "RIDES")])).filter(path => path.toUpperCase().endsWith(".LOG"));
const blobs = await Promise.all(files.map(path => readFile(path)));
const random = mulberry32(0x9c07);
const originalCache = new Map<number, Map<number, RideLogReading[]>>();
const originalReadings = (fileIndex: number): Map<number, RideLogReading[]> => {
  let byRecord = originalCache.get(fileIndex);
  if (!byRecord) {
    byRecord = readingsByRecord(blobs[fileIndex]);
    originalCache.set(fileIndex, byRecord);
  }
  return byRecord;
};
let truncations = 0;
let corruptions = 0;
for (let trial = 0; trial < 300; trial++) {
  const fileIndex = Math.floor(random() * blobs.length);
  const blob = blobs[fileIndex];
  const full = scanRecords(blob).records;
  const cut = Math.floor(random() * blob.length);
  // A record survives if every byte its CRC covers is present; its zero padding is never read.
  const survivors = full.filter(record => coveredEnd(record) <= cut);
  checkSurvivors(`truncated ${files[fileIndex]} at ${cut}`, blob.subarray(0, cut), survivors, originalReadings(fileIndex));
  truncations++;
}
for (let trial = 0; trial < 300; trial++) {
  const fileIndex = Math.floor(random() * blobs.length);
  const original = blobs[fileIndex];
  const full = scanRecords(original).records;
  // Bias towards the part of the file that holds records.
  const used = full.length > 0 ? full[full.length - 1].offset + full[full.length - 1].length : original.length;
  const sectorCount = Math.ceil((random() < 0.8 ? used : original.length) / SECTOR_BYTES);
  const sector = Math.floor(random() * sectorCount);
  const damagedBlob = Buffer.from(original);
  const mode = Math.floor(random() * 3);
  const start = sector * SECTOR_BYTES;
  const end = Math.min(start + SECTOR_BYTES, damagedBlob.length);
  if (mode === 0) {
    for (let index = start; index < end; index++) {
      damagedBlob[index] = Math.floor(random() * 256);
    }
  } else if (mode === 1) {
    damagedBlob.fill(0, start, end);
  } else {
    const source = Math.floor(random() * (original.length / SECTOR_BYTES)) * SECTOR_BYTES;
    original.copy(damagedBlob, start, source, source + (end - start));
  }
  const survivors = full.filter(record =>
    damagedBlob.subarray(record.offset, coveredEnd(record)).equals(original.subarray(record.offset, coveredEnd(record)))
  );
  checkSurvivors(`sector ${sector} of ${files[fileIndex]} (mode ${mode})`, damagedBlob, survivors, originalReadings(fileIndex));
  corruptions++;
}
// A torn final record: its last sector never reached the card.
for (let fileIndex = 0; fileIndex < blobs.length; fileIndex += 7) {
  const full = scanRecords(blobs[fileIndex]).records;
  if (full.length === 0) continue;
  const last = full[full.length - 1];
  const torn = Buffer.from(blobs[fileIndex]);
  torn.fill(0, last.offset + last.length - SECTOR_BYTES, last.offset + last.length);
  checkSurvivors(`torn last record of ${files[fileIndex]}`, torn, full.slice(0, -1), originalReadings(fileIndex));
}
console.log(`check-ride-log: ${truncations} truncations and ${corruptions} damaged sectors lost only what they touched`);

// 3. Import through cool-eva's db.ts.
const workDirectory = await mkdtemp(join(tmpdir(), "emcool-import-"));
try {
  const databasePath = join(workDirectory, "rides.db");
  const summary = await importRideLog([join(corpusDirectory, "RIDES")], databasePath, coolEvaDirectory, () => {});
  const requireFromCoolEva = createRequire(resolve(coolEvaDirectory, "package.json"));
  const Database = requireFromCoolEva("better-sqlite3");
  const database = new Database(databasePath, { readonly: true });
  const stored = database.prepare("SELECT COUNT(*) AS count FROM reading").get() as { count: number };
  if (stored.count !== expected.length || summary.readings !== expected.length) {
    fail(`rides.db holds ${stored.count} readings (import reported ${summary.readings}), expected ${expected.length}`);
  }
  const firstSeq = expected[0].seq;
  const sampleSeqs = [0, 0.01, 0.4, 1].map(fraction => expected[Math.floor((expected.length - 1) * fraction)].seq);
  const sample = database
    .prepare(
      `SELECT r.ts, s.key, r.value, r.seq, r.clock_trust AS trust, x.uid AS session
         FROM reading r JOIN signal s ON s.id = r.signal_id JOIN session x ON x.id = r.session_id
        WHERE r.seq IN (${sampleSeqs.join(",")})
        ORDER BY r.seq`
    )
    .all() as (ExpectedReading & { session: string })[];
  for (const row of sample) {
    const wanted = expected[row.seq - firstSeq];
    if (row.ts !== wanted.ts || row.key !== wanted.key || row.value !== wanted.value || row.trust !== wanted.trust || row.session !== session) {
      fail(`rides.db seq ${row.seq}: ${JSON.stringify(row)}, expected ${JSON.stringify(wanted)}`);
    }
  }
  if (sample.length !== 4) {
    fail(`rides.db sample found ${sample.length} of 4 rows`);
  }
  // Units and groups come from the metadata records on the card; they must be cool-eva's.
  const registry = await import(pathToFileURL(join(coolEvaDirectory, "src/can/registry.ts")).href);
  const registryByKey = new Map(
    (registry.SIGNALS as { key: string; unit: string; group: string }[]).map(signal => [signal.key, signal])
  );
  const selectSignal = database.prepare("SELECT unit, grp FROM signal WHERE key = ?");
  for (const key of new Set(sample.map(row => row.key))) {
    const stored = selectSignal.get(key) as { unit: string; grp: string } | undefined;
    const wanted = registryByKey.get(key);
    if (stored?.unit !== wanted?.unit || stored?.grp !== wanted?.group) {
      fail(`rides.db ${key} metadata is ${JSON.stringify(stored)}, cool-eva's registry says ${wanted?.unit}/${wanted?.group}`);
    }
  }
  database.close();
  console.log(`check-ride-log: imported ${stored.count} readings into rides.db through cool-eva's db.ts`);
} finally {
  await rm(workDirectory, { recursive: true, force: true });
}

console.log(`check-ride-log: ${failures} failures`);
process.exit(failures === 0 ? 0 : 1);

// A damaged copy must yield exactly the records it should still hold, each with the same
// readings as in the undamaged file: none lost beyond what the damage touched, none invented,
// none altered. Compared record by record, so a stale duplicate can't mask anything.
function checkSurvivors(
  description: string,
  blob: Buffer,
  survivors: ReturnType<typeof scanRecords>["records"],
  original: Map<number, RideLogReading[]>
): void {
  const got = readingsByRecord(blob);
  const wanted = new Set(survivors.map(record => record.index));
  for (const index of wanted) {
    const readingsNow = got.get(index);
    if (readingsNow === undefined) {
      fail(`${description}: lost record ${index}, which the damage did not touch`);
      return;
    }
    if (JSON.stringify(readingsNow) !== JSON.stringify(original.get(index))) {
      fail(`${description}: record ${index} reads back different readings`);
      return;
    }
  }
  for (const index of got.keys()) {
    if (!wanted.has(index)) {
      fail(`${description}: record ${index} survived damage that should have destroyed it`);
      return;
    }
  }
}

// The end of the bytes a record's CRC covers: header and payload, not the padding after.
function coveredEnd(record: ReturnType<typeof scanRecords>["records"][number]): number {
  return record.offset + HEADER_BYTES + Buffer.byteLength(record.body, "utf-8");
}

// Each intact record's readings, by record index; a duplicate index keeps its first copy.
function readingsByRecord(blob: Buffer): Map<number, RideLogReading[]> {
  const byRecord = new Map<number, RideLogReading[]>();
  for (const record of scanRecords(blob).records) {
    if (!byRecord.has(record.index)) {
      byRecord.set(record.index, readFileBlob("record", blob.subarray(record.offset, record.offset + record.length)).readings);
    }
  }
  return byRecord;
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
