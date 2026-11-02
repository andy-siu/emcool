// Laptop-side import of emcool's ride log: reads the RIDES/ folder off the microSD card and
// writes cool-eva's SQLite file through cool-eva's own src/db.ts, so Grafana and cool-eva's
// dashboards read it unchanged. The emcool counterpart of cool-eva scripts/decrypt-log.ts,
// without the decryption — the card is plain text.
//
//   node tools/import-ride-log.mts <RIDES dir or files…> [--out rides.db] [--force]
//
// Needs a cool-eva checkout with its dependencies installed (COOL_EVA_DIR, default ../cool-eva).

import { access } from "node:fs/promises";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { readRideLog, type FileReadout, type SignalMeta } from "./lib/ride-log-reader.mts";

interface CoolEvaDb {
  initDb(path: string, flushMs?: number): void;
  recordReading(
    ts: number,
    key: string,
    value: number,
    unit: string,
    group: string,
    source: string,
    session?: string,
    seq?: number,
    clockTrust?: string
  ): void;
  flushNow(): void;
  closeDb(): void;
}

export interface ImportSummary {
  readings: number;
  records: number;
  damagedRecords: number;
  duplicateRecords: number;
  untrustedReadings: number;
}

const emcoolRoot = join(dirname(fileURLToPath(import.meta.url)), "..");

// Imports into `outputPath` and returns what was read; the CLI below wraps it.
export async function importRideLog(
  inputs: string[],
  outputPath: string,
  coolEvaDirectory: string,
  log: (line: string) => void = console.log
): Promise<ImportSummary> {
  const db = (await import(pathToFileURL(join(coolEvaDirectory, "src/db.ts")).href)) as CoolEvaDb;
  const registry = await import(pathToFileURL(join(coolEvaDirectory, "src/can/registry.ts")).href);
  const registryByKey = new Map<string, SignalMeta & { key: string }>(
    (registry.SIGNALS as (SignalMeta & { key: string })[]).map(signal => [signal.key, signal])
  );

  const readouts = await readRideLog(inputs);
  db.initDb(resolve(outputPath));
  const summary: ImportSummary = { readings: 0, records: 0, damagedRecords: 0, duplicateRecords: 0, untrustedReadings: 0 };
  // Metadata from a session's earlier parts covers a later part whose opening records were lost.
  const sessionMeta = new Map<string, SignalMeta>();
  for (const readout of readouts) {
    for (const [key, meta] of readout.meta) {
      sessionMeta.set(key, meta);
    }
    for (const reading of readout.readings) {
      const meta = readout.meta.get(reading.key) ?? sessionMeta.get(reading.key) ?? registryByKey.get(reading.key);
      db.recordReading(
        reading.ts,
        reading.key,
        reading.value,
        meta?.unit ?? "",
        meta?.group ?? "misc",
        meta?.source ?? "stream",
        reading.session,
        reading.seq,
        reading.clockTrust
      );
      if (reading.clockTrust !== "satellite-backed") {
        summary.untrustedReadings++;
      }
    }
    db.flushNow();
    summary.readings += readout.readings.length;
    summary.records += readout.records.length;
    summary.damagedRecords += readout.damagedRecords;
    summary.duplicateRecords += readout.duplicateRecords;
    if (readout.records.length > 0 || readout.damagedRecords > 0) {
      log(describeFile(readout));
    }
  }
  db.closeDb();
  for (const line of describeMissingRecords(readouts)) {
    log(line);
  }
  return summary;
}

// Per session, the records that aren't on the card. Missing from the start usually means the
// card filled and the oldest parts were deleted to make room; a gap in the middle is loss.
function describeMissingRecords(readouts: FileReadout[]): string[] {
  const indexesBySession = new Map<string, number[]>();
  for (const readout of readouts) {
    for (const record of readout.records) {
      const indexes = indexesBySession.get(record.session) ?? [];
      indexes.push(record.index);
      indexesBySession.set(record.session, indexes);
    }
  }
  const lines: string[] = [];
  for (const [session, indexes] of indexesBySession) {
    indexes.sort((left, right) => left - right);
    const first = indexes[0];
    const last = indexes[indexes.length - 1];
    const gaps = last - first + 1 - indexes.length;
    if (first > 0) {
      lines.push(`session ${session}: its first ${first} records are not on the card — rotated off when it filled, or lost`);
    }
    if (gaps > 0) {
      lines.push(`session ${session}: ${gaps} records missing between ${first} and ${last} — lost`);
    }
  }
  return lines;
}

function describeFile(readout: FileReadout): string {
  const notes: string[] = [];
  if (readout.damagedRecords > 0) {
    notes.push(`${readout.damagedRecords} DAMAGED`);
  }
  if (readout.duplicateRecords > 0) {
    notes.push(`${readout.duplicateRecords} duplicate`);
  }
  if (readout.unrecognizedSectors > 0) {
    notes.push(`${readout.unrecognizedSectors} unrecognized sectors`);
  }
  const name = `${basename(dirname(readout.path))}/${basename(readout.path)}`;
  return `${name}: ${readout.records.length} records, ${readout.readings.length} readings${notes.length ? ` (${notes.join(", ")})` : ""}`;
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const inputs: string[] = [];
  let outputPath = "rides.db";
  let force = false;
  for (let index = 0; index < args.length; index++) {
    const arg = args[index];
    if (arg === "--force") {
      force = true;
    } else if (arg === "--out") {
      outputPath = args[++index] ?? fail("--out needs a filename");
    } else if (arg.startsWith("--")) {
      fail(`unknown option ${arg}`);
    } else {
      inputs.push(arg);
    }
  }
  if (inputs.length === 0) {
    fail("usage: import-ride-log.mts <RIDES dir or files…> [--out rides.db] [--force]");
  }
  const outputExists = await access(outputPath).then(
    () => true,
    () => false
  );
  if (outputExists && !force) {
    fail(`${resolve(outputPath)} already exists. Delete it, choose another --out, or pass --force to append.`);
  }
  const coolEvaDirectory = resolve(process.env.COOL_EVA_DIR ?? join(emcoolRoot, "../cool-eva"));
  const summary = await importRideLog(inputs, outputPath, coolEvaDirectory);
  console.log(`\n${summary.readings} readings from ${summary.records} records → ${resolve(outputPath)}`);
  if (summary.untrustedReadings > 0) {
    console.warn(
      `\n⚠️  ${summary.untrustedReadings} reading(s) were stamped while the clock could not be trusted. ` +
        "Their `ts` is NOT corrected; `seq` still orders them. In rides.db: reading.clock_trust."
    );
  }
  if (summary.damagedRecords > 0) {
    console.error(`\n${summary.damagedRecords} record(s) were damaged and skipped — that data is lost, the rest is intact.`);
    process.exit(2);
  }
}

function fail(message: string): never {
  console.error(message);
  process.exit(1);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  await main();
}
