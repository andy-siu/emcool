// Reads emcool ride-log files off the microSD card: finds every intact record, checks it
// with zlib's CRC-32 (independent of the firmware's own), and parses its body the way cool-eva
// scripts/decrypt-log.ts parses a decrypted segment. The record format is in
// components/emcool_ride_log/include/emcool/ride_log/record.hpp and docs/ride-log-format.md.
//
// Anything that is not an intact record — the zero-filled tail of a part, a record torn by a
// power cut, stale data — is stepped over a 512-byte sector at a time, so one bad sector
// costs at most the record it belongs to.

import { readdir, readFile, stat } from "node:fs/promises";
import { join } from "node:path";
import { crc32 } from "node:zlib";

export const SECTOR_BYTES = 512;
export const HEADER_BYTES = 32;
export const MAX_PAYLOAD_BYTES = 8192 - HEADER_BYTES;
export const MAGIC = Buffer.from("EMCOOL01");

export type SignalSource = "stream" | "poll" | "sensor";

export interface SignalMeta {
  unit: string;
  group: string;
  source: SignalSource;
}

export interface RideLogRecord {
  offset: number;
  length: number;
  index: number;
  session: string;
  body: string;
}

export interface RideLogReading {
  ts: number;
  key: string;
  value: number;
  session: string;
  seq: number;
  clockTrust: string | undefined;
}

export interface FileScan {
  records: RideLogRecord[];
  // A sector that starts with the magic but fails its length or CRC: a torn or damaged record.
  damagedRecords: number;
  // A non-zero sector that isn't a record: stale or foreign data. Zero sectors are the
  // expected prefill and are not counted.
  unrecognizedSectors: number;
}

export interface FileReadout extends FileScan {
  path: string;
  readings: RideLogReading[];
  meta: Map<string, SignalMeta>;
  duplicateRecords: number;
}

export function scanRecords(blob: Buffer): FileScan {
  const records: RideLogRecord[] = [];
  let damagedRecords = 0;
  let unrecognizedSectors = 0;
  let offset = 0;
  while (offset + HEADER_BYTES <= blob.length) {
    if (!blob.subarray(offset, offset + MAGIC.length).equals(MAGIC)) {
      if (!isZero(blob, offset, Math.min(offset + SECTOR_BYTES, blob.length))) {
        unrecognizedSectors++;
      }
      offset += SECTOR_BYTES;
      continue;
    }
    const payloadLength = blob.readUInt32LE(offset + 20);
    const end = offset + HEADER_BYTES + payloadLength;
    if (payloadLength > MAX_PAYLOAD_BYTES || end > blob.length || blob.readUInt32LE(offset + 28) !== 0) {
      damagedRecords++;
      offset += SECTOR_BYTES;
      continue;
    }
    const expectedCrc = crc32(blob.subarray(offset + HEADER_BYTES, end), crc32(blob.subarray(offset, offset + 24)));
    if (blob.readUInt32LE(offset + 24) !== expectedCrc) {
      damagedRecords++;
      offset += SECTOR_BYTES;
      continue;
    }
    const length = Math.ceil((HEADER_BYTES + payloadLength) / SECTOR_BYTES) * SECTOR_BYTES;
    records.push({
      offset,
      length,
      index: blob.readUInt32LE(offset + 16),
      session: blob.subarray(offset + 8, offset + 16).toString("hex"),
      body: blob.subarray(offset + HEADER_BYTES, end).toString("utf-8"),
    });
    offset += length;
  }
  return { records, damagedRecords, unrecognizedSectors };
}

// One file's readings, in record order. `seen` dedupes records across files — a record can
// survive twice, e.g. as stale data in the zero-filled tail of a later part.
export function readFileBlob(path: string, blob: Buffer, seen: Set<string> = new Set()): FileReadout {
  const scan = scanRecords(blob);
  const readings: RideLogReading[] = [];
  const meta = new Map<string, SignalMeta>();
  let duplicateRecords = 0;
  let damagedRecords = scan.damagedRecords;
  const kept: RideLogRecord[] = [];
  for (const record of scan.records) {
    const identity = `${record.session}:${record.index}`;
    if (seen.has(identity)) {
      duplicateRecords++;
      continue;
    }
    seen.add(identity);
    if (!readBody(record, readings, meta)) {
      damagedRecords++;
      continue;
    }
    kept.push(record);
  }
  return { ...scan, records: kept, damagedRecords, path, readings, meta, duplicateRecords };
}

export async function readRideLog(inputs: string[]): Promise<FileReadout[]> {
  const seen = new Set<string>();
  const readouts: FileReadout[] = [];
  for (const path of await collectFiles(inputs)) {
    readouts.push(readFileBlob(path, await readFile(path), seen));
  }
  return readouts;
}

// Every regular file under the inputs, sorted, so boots and parts come in order.
export async function collectFiles(inputs: string[]): Promise<string[]> {
  const files: string[] = [];
  for (const input of inputs) {
    const info = await stat(input);
    if (info.isDirectory()) {
      const entries = (await readdir(input)).filter(entry => !entry.startsWith("."));
      files.push(...(await collectFiles(entries.map(entry => join(input, entry)))));
    } else if (info.isFile()) {
      files.push(input);
    }
  }
  return files.sort();
}

// cool-eva decrypt-log.ts readBody(): a header object sets the session and trust for the
// rows after it and contributes any `signals` metadata; an array is a row. Returns false if
// the body doesn't parse or claims a different session from its record header.
function readBody(record: RideLogRecord, readings: RideLogReading[], meta: Map<string, SignalMeta>): boolean {
  const parsedLines: unknown[] = [];
  try {
    for (const line of record.body.split("\n")) {
      if (line.length > 0) {
        parsedLines.push(JSON.parse(line));
      }
    }
  } catch {
    return false;
  }
  // Collected locally so a record rejected part-way through contributes nothing.
  const recordReadings: RideLogReading[] = [];
  const recordMeta = new Map<string, SignalMeta>();
  let session: string | undefined;
  let clockTrust: string | undefined;
  for (const parsed of parsedLines) {
    if (Array.isArray(parsed)) {
      const [ts, key, value, seq] = parsed as [number, string, number, number];
      if (session === undefined) {
        return false;
      }
      recordReadings.push({ ts, key, value, session, seq, clockTrust });
      continue;
    }
    const header = parsed as { session?: string; trust?: string; signals?: Record<string, [string, string, SignalSource]> };
    if (header.session !== record.session) {
      return false;
    }
    session = header.session;
    clockTrust = header.trust;
    for (const [key, [unit, group, source]] of Object.entries(header.signals ?? {})) {
      recordMeta.set(key, { unit, group, source });
    }
  }
  readings.push(...recordReadings);
  for (const [key, signalMeta] of recordMeta) {
    meta.set(key, signalMeta);
  }
  return true;
}

function isZero(blob: Buffer, start: number, end: number): boolean {
  for (let index = start; index < end; index++) {
    if (blob[index] !== 0) {
      return false;
    }
  }
  return true;
}
