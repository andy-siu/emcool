// rides.db: the SQLite file the laptop analyses rides in (Grafana reads it as is). Written with
// Node's built-in SQLite (node:sqlite), so importing a ride log needs nothing installed beyond Node.
//
// Long schema, one row per logged reading:
//   signal   one row per signal key: its unit, group and source
//   session  one row per boot of the bike that produced readings (its random session id)
//   reading  (ts, signal_id, value, session_id, seq, clock_trust)
//   info     key/value notes
//
// `ts` is the bike's wall clock and nothing more: before GPS time is trusted it can be 1970, and a
// clock step moves it, so ORDER BY ts is not write order. (session_id, seq) is: `seq` counts every
// reading of a boot. `clock_trust` says what the clock was worth when `ts` was taken.
//
// An existing rides.db is opened and appended to, so imports can accumulate; a `reading` table from
// before session_id/seq/clock_trust existed gets those columns added (as NULL for its old rows).

import { DatabaseSync, type StatementSync } from "node:sqlite";

export type SignalSource = "stream" | "poll" | "sensor";

export class RidesDb {
  #db: DatabaseSync;
  #insertReading: StatementSync;
  #insertSignal: StatementSync;
  #selectSignal: StatementSync;
  #insertSession: StatementSync;
  #selectSession: StatementSync;
  #signalIds = new Map<string, number>();
  #sessionIds = new Map<string, number>();
  #pending = 0;

  constructor(path: string) {
    this.#db = new DatabaseSync(path);
    this.#db.exec("PRAGMA journal_mode = WAL");
    this.#db.exec(`
      CREATE TABLE IF NOT EXISTS signal (
        id     INTEGER PRIMARY KEY,
        key    TEXT UNIQUE,
        unit   TEXT,
        grp    TEXT,
        source TEXT
      );
      CREATE TABLE IF NOT EXISTS session (
        id  INTEGER PRIMARY KEY,
        uid TEXT UNIQUE
      );
      CREATE TABLE IF NOT EXISTS reading (
        ts          INTEGER NOT NULL,
        signal_id   INTEGER NOT NULL REFERENCES signal(id),
        value       REAL NOT NULL,
        session_id  INTEGER REFERENCES session(id),
        seq         INTEGER,
        clock_trust TEXT
      );
      CREATE INDEX IF NOT EXISTS idx_reading_sig_ts ON reading(signal_id, ts);
      CREATE TABLE IF NOT EXISTS info (
        key   TEXT PRIMARY KEY,
        value TEXT,
        ts    INTEGER
      );
    `);
    this.#addOrderingColumns();
    this.#insertReading = this.#db.prepare(
      "INSERT INTO reading (ts, signal_id, value, session_id, seq, clock_trust) VALUES (?, ?, ?, ?, ?, ?)"
    );
    this.#insertSignal = this.#db.prepare(
      "INSERT INTO signal (key, unit, grp, source) VALUES (?, ?, ?, ?) ON CONFLICT(key) DO NOTHING"
    );
    this.#selectSignal = this.#db.prepare("SELECT id FROM signal WHERE key = ?");
    this.#insertSession = this.#db.prepare("INSERT INTO session (uid) VALUES (?) ON CONFLICT(uid) DO NOTHING");
    this.#selectSession = this.#db.prepare("SELECT id FROM session WHERE uid = ?");
    this.#db.exec("BEGIN");
  }

  recordReading(
    ts: number,
    key: string,
    value: number,
    unit: string,
    group: string,
    source: SignalSource,
    session?: string,
    seq?: number,
    clockTrust?: string
  ): void {
    this.#insertReading.run(
      ts,
      this.#signalId(key, unit, group, source),
      value,
      session === undefined ? null : this.#sessionId(session),
      seq ?? null,
      clockTrust ?? null
    );
    // Committed in batches: one transaction per reading would take minutes for an hour's ride.
    if (++this.#pending >= 50_000) this.flush();
  }

  // Commits what has been recorded so far.
  flush(): void {
    this.#db.exec("COMMIT");
    this.#db.exec("BEGIN");
    this.#pending = 0;
  }

  close(): void {
    this.#db.exec("COMMIT");
    this.#db.close();
  }

  #signalId(key: string, unit: string, group: string, source: SignalSource): number {
    const cached = this.#signalIds.get(key);
    if (cached !== undefined) return cached;
    this.#insertSignal.run(key, unit, group, source);
    const id = Number((this.#selectSignal.get(key) as { id: number | bigint }).id);
    this.#signalIds.set(key, id);
    return id;
  }

  #sessionId(uid: string): number {
    const cached = this.#sessionIds.get(uid);
    if (cached !== undefined) return cached;
    this.#insertSession.run(uid);
    const id = Number((this.#selectSession.get(uid) as { id: number | bigint }).id);
    this.#sessionIds.set(uid, id);
    return id;
  }

  // A rides.db made before session_id, seq and clock_trust existed: add them. ADD COLUMN only edits
  // the schema, so this is instant however big the file, and the old rows read NULL, which is true.
  #addOrderingColumns(): void {
    const present = new Set(
      (this.#db.prepare("SELECT name FROM pragma_table_info('reading')").all() as { name: string }[]).map(
        column => column.name
      )
    );
    for (const [name, definition] of [
      ["session_id", "INTEGER REFERENCES session(id)"],
      ["seq", "INTEGER"],
      ["clock_trust", "TEXT"],
    ]) {
      if (!present.has(name)) this.#db.exec(`ALTER TABLE reading ADD COLUMN ${name} ${definition}`);
    }
  }
}
