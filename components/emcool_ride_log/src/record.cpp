#include "emcool/ride_log/record.hpp"

#include <algorithm>
#include <cstring>

#include "emcool/ride_log/crc32.hpp"

namespace emcool::ride_log {

namespace {

constexpr std::size_t kCrcOffset = 24;

void putLittleEndian32(std::uint8_t* destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value);
  destination[1] = static_cast<std::uint8_t>(value >> 8);
  destination[2] = static_cast<std::uint8_t>(value >> 16);
  destination[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t getLittleEndian32(const std::uint8_t* source) {
  return std::uint32_t{source[0]} | (std::uint32_t{source[1]} << 8) | (std::uint32_t{source[2]} << 16) |
         (std::uint32_t{source[3]} << 24);
}

std::uint32_t recordCrc(const std::uint8_t* record, std::size_t payloadLength) {
  const std::uint32_t headerCrc = crc32(record, kCrcOffset);
  return crc32(record + kRecordHeaderBytes, payloadLength, headerCrc);
}

}  // namespace

SessionId SessionId::fromBytes(const std::array<std::uint8_t, 8>& bytes) {
  constexpr char kDigits[] = "0123456789abcdef";
  SessionId session = {bytes, {}};
  for (std::size_t index = 0; index < bytes.size(); index++) {
    session.hex[2 * index] = kDigits[bytes[index] >> 4];
    session.hex[2 * index + 1] = kDigits[bytes[index] & 0x0f];
  }
  session.hex[16] = '\0';
  return session;
}

std::size_t paddedRecordLength(std::size_t payloadLength) {
  const std::size_t unpadded = kRecordHeaderBytes + payloadLength;
  return (unpadded + kSectorBytes - 1) / kSectorBytes * kSectorBytes;
}

std::size_t finalizeRecord(std::uint8_t* record, const SessionId& session, std::uint32_t recordIndex,
                           std::size_t payloadLength) {
  std::memcpy(record, kRecordMagic.data(), kRecordMagic.size());
  std::memcpy(record + 8, session.bytes.data(), session.bytes.size());
  putLittleEndian32(record + 16, recordIndex);
  putLittleEndian32(record + 20, static_cast<std::uint32_t>(payloadLength));
  putLittleEndian32(record + 28, 0);
  putLittleEndian32(record + kCrcOffset, recordCrc(record, payloadLength));
  const std::size_t length = paddedRecordLength(payloadLength);
  std::memset(record + kRecordHeaderBytes + payloadLength, 0, length - kRecordHeaderBytes - payloadLength);
  return length;
}

std::vector<ScannedRecord> scanRecords(std::span<const std::uint8_t> bytes) {
  std::vector<ScannedRecord> records;
  std::size_t offset = 0;
  while (offset + kRecordHeaderBytes <= bytes.size()) {
    const std::uint8_t* header = bytes.data() + offset;
    const std::uint32_t payloadLength = getLittleEndian32(header + 20);
    const bool plausible = std::equal(kRecordMagic.begin(), kRecordMagic.end(), header) &&
                           payloadLength <= kMaxPayloadBytes &&
                           offset + kRecordHeaderBytes + payloadLength <= bytes.size();
    if (plausible && getLittleEndian32(header + kCrcOffset) == recordCrc(header, payloadLength)) {
      ScannedRecord record = {};
      record.offset = offset;
      record.length = paddedRecordLength(payloadLength);
      record.index = getLittleEndian32(header + 16);
      std::copy(header + 8, header + 16, record.session.begin());
      record.payload = bytes.subspan(offset + kRecordHeaderBytes, payloadLength);
      records.push_back(record);
      offset += record.length;
      continue;
    }
    offset += kSectorBytes;
  }
  return records;
}

}  // namespace emcool::ride_log
