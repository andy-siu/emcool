#pragma once

// The on-card record. The card holds plain files on FAT32, and the bike cuts power without
// warning, so every record frames and checks itself:
//
//   offset  size  field (integers little-endian)
//   0       8     magic "EMCOOL01"
//   8       8     session id, random per boot
//   16      4     record index within the session: 0, 1, 2, … (orders records, exposes gaps)
//   20      4     payload length
//   24      4     CRC-32 of bytes 0-23 followed by the payload
//   28      4     zero
//   32      n     payload: UTF-8 JSON lines, cool-eva's v3 segment body (one header object,
//                 then [ts, key, value, seq] rows)
//                 zero padding to the next 512-byte sector
//
// Records start on sector boundaries, so writing one never rewrites a sector that already
// holds a committed record, and a reader that hits a torn or stale sector resyncs at the next
// boundary. The layout and the reasons for it: docs/ride-log-format.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace emcool::ride_log {

inline constexpr std::size_t kSectorBytes = 512;
inline constexpr std::size_t kRecordHeaderBytes = 32;
// A whole record, padding included. Two of these are the decode side's entire buffer.
inline constexpr std::size_t kRecordCapacityBytes = 8192;
inline constexpr std::size_t kMaxPayloadBytes = kRecordCapacityBytes - kRecordHeaderBytes;
inline constexpr std::array<std::uint8_t, 8> kRecordMagic = {'E', 'M', 'C', 'O', 'O', 'L', '0', '1'};

static_assert(kRecordCapacityBytes % kSectorBytes == 0);

struct SessionId {
  std::array<std::uint8_t, 8> bytes;
  // Lower-case hex, as cool-eva writes its session ids, NUL-terminated.
  std::array<char, 17> hex;

  static SessionId fromBytes(const std::array<std::uint8_t, 8>& bytes);
};

std::size_t paddedRecordLength(std::size_t payloadLength);

// Writes the header in front of the `payloadLength` bytes already at
// `record + kRecordHeaderBytes`, zero-pads to the sector boundary, and returns the total length.
std::size_t finalizeRecord(std::uint8_t* record, const SessionId& session, std::uint32_t recordIndex,
                           std::size_t payloadLength);

struct ScannedRecord {
  std::size_t offset;
  std::size_t length;
  std::uint32_t index;
  std::array<std::uint8_t, 8> session;
  std::span<const std::uint8_t> payload;
};

// Every intact record in `bytes`, in order. Anything else — zeros, a torn record, stale data —
// is stepped over a sector at a time. The laptop has its own reader (tools/lib); this one is
// for the host checks.
std::vector<ScannedRecord> scanRecords(std::span<const std::uint8_t> bytes);

}  // namespace emcool::ride_log
