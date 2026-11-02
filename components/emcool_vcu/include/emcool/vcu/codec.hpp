#pragma once

// The bytes of a VCU parameter read, ported from cool-eva src/vcu/param-codec.ts, the framing half
// of src/vcu/multiframe-codec.ts, and src/diagnostics/extended-iso-tp.ts. Pure.
//
// KWP2000 over ISO-TP with EXTENDED addressing: byte 0 of every frame is an address (0xA8 or 0xA9
// out, 0xF1 — the tester — back), so a frame carries 6 payload bytes, not 7. Requests go to
// 0x7C0 for both micros, replies come on 0x7E0, inside the OBD reply range; the 0xF1 is what
// tells a KWP reply from an OBD one.
//
// READ-ONLY, structurally: the request encoder is a closed set of three (start a session, tester
// present, read by common identifier), and the service byte it emits is checked against that set
// again. Nothing here can say a write.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/vcu/tables.hpp"

namespace emcool::vcu {

inline constexpr std::uint8_t kTesterAddress = 0xf1;
inline constexpr std::uint32_t kKwpRequestCanId = 0x7c0;
inline constexpr std::uint32_t kKwpResponseCanId = 0x7e0;

std::uint8_t targetAddress(Micro micro);
const char* microName(Micro micro);
std::optional<Micro> microNamed(std::string_view name);  // "A8" or "A9", any case
int recordLengthFor(StorageType type);
const char* storageTypeName(StorageType type);

// "0A FF 3C": cool-eva's toHex.
std::string toHex(std::span<const std::uint8_t> bytes);

// --- Requests -------------------------------------------------------------------------------

enum class RequestKind : std::uint8_t { kStartSession, kTesterPresent, kReadParameter };
struct VcuRequest {
  RequestKind kind;
  int bank = 0;
  int index = 0;
};

// (bank << 12) | index, for a bank 0-15 and an index 0-4095; nullopt outside them.
std::optional<std::uint16_t> identifierFor(int bank, int index);
// `10 81`, `3E`, or `22 hi lo`.
std::vector<std::uint8_t> buildRequestPayload(const VcuRequest& request);
// One single frame: [address][length][payload][zero padding].
std::array<std::uint8_t, 8> buildRequestFrame(Micro micro, const VcuRequest& request);

// --- Replies to a single-frame exchange -----------------------------------------------------

struct VcuFrame {
  enum class Kind : std::uint8_t { kPayload, kMultiFrame, kIgnored };
  Kind kind = Kind::kIgnored;
  std::vector<std::uint8_t> payload;
  int totalLength = 0;
  std::string reason;
};
VcuFrame parseResponseFrame(std::span<const std::uint8_t> frame);
bool isSessionOpened(std::span<const std::uint8_t> payload);

struct VcuParameterReply {
  enum class Kind : std::uint8_t { kRecord, kRefused, kIdentifierMismatch, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  std::vector<std::uint8_t> record;
  int service = 0;
  int negativeResponseCode = 0;
  std::string description;
  int expected = 0;
  int received = 0;
  std::string reason;
};
VcuParameterReply decodeParameterReply(std::span<const std::uint8_t> payload, int expectedIdentifier);

// --- Reading a record -----------------------------------------------------------------------

// What the record's width and sign come from: a parameter table row, or the A8 firmware table.
struct RecordEncoding {
  StorageType type;
  std::optional<bool> isSigned;  // nullopt: not known, so no value
  const char* describedBy;       // null: "the name table"
};

struct VcuParameterValue {
  std::string rawHex;
  double unsignedValue;
  double signedValue;
  std::optional<double> value;
  bool widthMismatch;
};
VcuParameterValue interpretRecord(std::span<const std::uint8_t> record, const std::optional<RecordEncoding>& encoding);
std::string describeWidthMismatch(std::size_t recordLength, const RecordEncoding& encoding);

// --- Multi-frame framing --------------------------------------------------------------------

// A request payload as frames: one single frame up to 6 bytes, else a first frame and
// consecutive frames numbered from 0. Empty, or over 4095 bytes: none.
std::vector<std::array<std::uint8_t, 8>> segmentRequestPayload(Micro micro, std::span<const std::uint8_t> payload);
// Clear to send, block size 0xFF, no gap.
std::array<std::uint8_t, 8> buildFlowControlFrame(Micro micro);

struct VcuFlowControl {
  enum class Status : std::uint8_t { kClearToSend, kWait, kOverflow, kUnrecognised };
  Status status;
  int blockSize = 0;
  int separationTimeMs = 0;
  int flowStatus = 0;
};
std::optional<VcuFlowControl> parseFlowControlFrame(std::span<const std::uint8_t> frame);

struct VcuMultiFrameReply {
  enum class Kind : std::uint8_t { kPositive, kRefused, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  int service = 0;
  std::vector<std::uint8_t> body;
  int negativeResponseCode = 0;
  std::string description;
  std::string reason;
};
VcuMultiFrameReply decodeMultiFrameReply(std::span<const std::uint8_t> payload, int expectedService);

// --- The multi-frame services (multiframe-codec.ts) -----------------------------------------
//
// Two, both reads: `17 <component>` ReadDTCInformation for one freeze frame, and `18 02 FF FF`
// ReadDTCByStatus for the list of components that have one. Like the parameter requests, the
// service byte is checked against those two again before anything is sent.

struct VcuMultiFrameRequest {
  enum class Kind : std::uint8_t { kReadFreezeFrame, kListStoredDtcs };
  Kind kind;
  int component = 0;  // kReadFreezeFrame, 1-63
};
// Empty for a component outside 1-63.
std::vector<std::uint8_t> encodeMultiFrameRequestPayload(const VcuMultiFrameRequest& request);
int expectedResponseService(const VcuMultiFrameRequest& request);

struct StoredDtcRecord {
  int code;
  int status;
};
struct VcuStoredDtcList {
  int declaredCount = 0;
  std::vector<StoredDtcRecord> records;
  int paddingRecords = 0;
  std::string trailingHex;
  bool truncated = false;
};
// The body of a `58` reply: a count, then 3-byte (code, status) records.
VcuStoredDtcList decodeStoredDtcList(std::span<const std::uint8_t> body);

// --- Extended-addressing reassembly ---------------------------------------------------------

struct ExtendedIsoTpResult {
  enum class Status : std::uint8_t { kIncomplete, kFlowControlRequired, kComplete, kIgnored, kAbandoned };
  Status status;
  int totalLength = 0;
  std::vector<std::uint8_t> payload;
  std::string reason;
};

class ExtendedIsoTpReassembler {
 public:
  // cool-eva's default cap is 64; a parameter read uses 32, the multi-frame services 256.
  explicit ExtendedIsoTpReassembler(int maxPayloadBytes = 64);
  ExtendedIsoTpResult push(std::span<const std::uint8_t> frame);
  void reset();

 private:
  ExtendedIsoTpResult malformed(std::string reason);

  int maxPayloadBytes_;
  int maxFrames_;
  std::vector<std::uint8_t> payload_;
  int filled_ = 0;
  int expectedSequenceNumber_ = 0;
  int frames_ = 0;
  bool receiving_ = false;
};

int maxFramesFor(int maxPayloadBytes);

}  // namespace emcool::vcu
