# The ride log on the microSD card

emcool keeps its ride log on the microSD card:

- The card holds plain files on FAT32. The log isn't encrypted.
- Position, heading and altitude are never written (`ride_log_policy.hpp`).
- The framing is built for a card that loses power without warning.

The laptop import (`tools/import-ride-log.mts`) reads the card and writes `rides.db` with Node's built-in SQLite, so it needs Node 22.13 or later and nothing else. Grafana reads `rides.db` as it is.

Code: `components/emcool_ride_log/` (firmware and host), `tools/lib/ride-log-reader.mts` and `tools/lib/rides-db.mts` (laptop), and `main/ride_log_task.cpp` (the card).

## On the card

```
RIDES/
  00001/            one directory per boot, numbered one past the highest already there
    PART0001.LOG    8 MB each; the next is created and zero-filled while this one fills
    PART0002.LOG
  00002/
```

All the names are 8.3, so FAT needs no long-filename entries.

**Why a directory per boot:** FatFs rewrites a file's directory entry on every `fsync`, to update its size and time, and an entry shares its 512-byte sector with up to 15 others. With a directory per boot, a torn write of that sector can only damage this boot's parts, never an earlier ride's.

### Preparing a card

The firmware mounts FAT32 only: ESP-IDF builds FatFs without exFAT. Macs format cards larger than 32 GB as exFAT by default, so either use a card of 32 GB or less, or format it explicitly:

```bash
diskutil list                                        # find the card: /dev/diskN — check this
diskutil eraseDisk FAT32 EMCOOL MBRFormat /dev/diskN  # erases that whole disk
```

The firmware never formats a card itself. A card it can't mount is left untouched.

## Records

Every write is one record, starting on a sector boundary:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `EMCOOL01` |
| 8 | 8 | session id, random per boot |
| 16 | 4 | record index within the session: 0, 1, 2, … |
| 20 | 4 | payload length |
| 24 | 4 | CRC-32 (zlib's) of bytes 0–23, then the payload |
| 28 | 4 | zero |
| 32 | n | payload |
| | | zero padding to the next 512-byte boundary |

All integers are little-endian.

The **payload** is a v3 segment body, in plain text: a header line `{"v":3,"session":…,"trust":…}`, then `[ts, key, value, seq]` rows. Each part opens with records whose header carries a `signals` map, giving every logged signal's unit, group and source. That way each part file can be read on its own even if the signal registry is later renamed. Segment records don't repeat it.

Why each piece of the framing is there:

- **Sector alignment.** Writing a record never rewrites a sector that already holds a committed one. If records shared sectors, appending would read-modify-write the previous record's last sector, and a power cut there could take that record with it.
- **CRC.** A torn or damaged record is detected, not misread. The reader then resyncs at the next sector boundary, so one bad sector costs at most the record it belongs to.
- **Session id and record index.** These identify each record. The reader drops duplicates, such as a stale copy left in the zero-filled tail of a part, and a gap in the index shows exactly what was lost.
- **`seq` on every row.** This orders readings whatever the clock said. Every reading takes a number even when it is dropped, so drops leave gaps too.

## When a segment is written

The decode task fills one of two 8 KB buffers, and the writer task writes it with `pwrite` and `fsync`. A segment is sealed when any of these happens:

- **The next row doesn't fit.** That is about a second of riding, at a measured 120–170 readings a second.
- **The clock's trust changes.** A segment carries exactly one trust: never-synced, satellite-backed or contested.
- **It is 5 s old.**
- **The bike parks.** `vehicle_state_can` enters 60, at most once per 5 s. Parking is the only warning before the power goes.

If the writer still holds both buffers, new readings are dropped and counted. The 10 s status line shows the count.

### Space is reserved ahead

A part's full 8 MB is zero-filled before it is needed, in 64 KB steps between records. So FAT allocates clusters ahead of time, and writing a record only overwrites zeros in space the file already owns.

ESP-IDF's helper for reserving contiguous space (`esp_vfs_fat_create_contiguous_file`) needs `f_expand`, which v6.1 compiles out (`FF_USE_EXPAND 0`). And its `ftruncate` zero-fills with the filesystem lock held, which would stall the live writes for seconds. So the writer zero-fills the next part itself, between records.

## What a power cut costs

**Lost:**

- The open segment: up to 5 s of readings, or about 1 s while riding.
- At most one sealed segment still waiting for the writer.

**Not lost:** every committed record. Nothing after a record is written ever touches its sectors.

**Still exposed:**

- **Directory entry and FAT.** Each `fsync` rewrites this boot's directory entry, and background zero-filling writes FAT sectors. A torn write there could make a part unreachable through the filesystem.
  - The records themselves are self-framing and sector-aligned, so in principle they can still be found by scanning a raw image of the card for the magic.
  - The reader can't do that for a whole card yet: it loads each file into memory whole, so a full-size image would need a streaming mode, which isn't built.
  - A record that straddles two fragments of a fragmented file couldn't be recovered that way.
- **The card's own controller.** An SD card can corrupt data during a power loss whatever the filesystem does. The fix would be hardware: a hold-up capacitor, plus sensing the supply dropping. Nothing like that is fitted.

## When the card is full

The oldest parts are deleted to make room, first in, first out:

- Before creating a part, the writer checks free space (`esp_vfs_fat_info`). FatFs trusts the card's FSInfo count, so this is cheap.
- If less than a whole part plus 1 MB is free, it deletes the oldest part: the lowest boot number, then its lowest part number. It repeats until the new part fits.
- A write that hits ENOSPC anyway, because the free-space figure was optimistic, rotates the same way and retries.
- When a boot's last part goes, its directory goes with it, unless something else is in it.
- This boot's own earlier parts are fair game once every older boot is gone. Its current part, and the one being prepared, never are.

**Only the ride log is ever deleted:** files named `PART<digits>.LOG` inside `RIDES/<digits>/`. Anything else on the card is left alone, including files you put in a boot directory.

**A card full of other files:** if nothing of the ride log is left to delete, logging stops and the console says so. Logging also stops if the next part still won't fit when the current one fills, until there's room.

**Every part stands alone.** Each part opens with its own signal metadata, so deleting old parts never leaves a newer one unreadable.

**Over Bluetooth:** the dashboard's `/dl` downloads the newest parts as one file, each cut where its records end (found by bisecting for the first all-zero sector, then probing past it). Records frame themselves, so the reader takes the file just as it takes the card's own parts. See [`ble.md`](ble.md).

**On import:** `tools/import-ride-log.mts` says how many of a session's records aren't on the card. Missing from the start usually means rotated away; a gap in the middle means lost.

The 10 s status line counts how many old parts have been deleted for room.

## Verified, and not

**Verified on the laptop** by `tools/check.sh`:

- **Exact round trip.** The decoder corpus (471,056 readings) goes through the C++ pipeline onto disk, and is read back exactly: ts, key, value, seq, session and trust.
- **Damage tests.** 300 random truncations, 300 damaged sectors (garbage, zeros or stale copies) and torn final records each lose exactly the records they touch, and never produce a wrong reading.
- **Import.** The corpus imports into `rides.db`, and sampled rows match, with the reference implementation's units and groups.
- **The tests can fail.** With the reader's CRC check disabled, the damage tests fail.
- **Rotation rules.** Old boots go first, then this boot's oldest parts, in order. Non-part files, non-numbered directories, and a boot directory holding a foreign file are left alone. A card full of foreign files stops logging and deletes nothing.
- **A full card, end to end.** The same corpus runs on a simulated 6 MB card and on a real 8 MB FAT image mounted by macOS, where writes genuinely run out of room. Both keep exactly the newest readings, contiguous, with nothing missing in between, and both import.

**Not verified yet:**

- Anything on the ESP32 itself: the mount, write speed, a real power cut, or a real card's behaviour.
- The firmware build compiles the card code but has not run it.
