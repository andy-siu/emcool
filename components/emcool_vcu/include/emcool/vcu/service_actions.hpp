#pragma once

// The pure half of the service actions, ported from cool-eva src/vcu/service-actions.ts: reading
// and interpreting the on-bike service stamp, deciding whether this Pi's (bike's) clock is fit to
// copy, the 0x120 RTC-sync frame, and the OBD Mode 04 clear-DTCs request. Numbers in, numbers out.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace emcool::vcu {

// --- The service stamp ----------------------------------------------------------------------

struct ServiceStampIdentifier {
  const char* field;
  int index;
  int identifier;
};
// The four bank-1 WORDs on A8 that hold the last-service stamp, in read order.
std::span<const ServiceStampIdentifier> serviceStampIdentifiers();
inline constexpr const char* kServiceStampMicro = "A8";  // not derivable from any index range

struct ServiceStampRaw {
  int dateLow = 0;
  int dateHigh = 0;
  int odometerLow = 0;
  int odometerHigh = 0;
};

struct ServiceStamp {
  ServiceStampRaw raw;
  double dateSeconds = 0;           // seconds since 2000-01-01 UTC, as stored
  std::optional<std::string> dateIso;  // ISO instant, or nullopt when not representable
  double odometer = 0;              // km or miles, per market
  std::optional<std::string> implausible;  // why it should not be believed, or nullopt
};
// nowMs is used only to say "that is in the future".
ServiceStamp interpretServiceStamp(const ServiceStampRaw& raw, double nowMs);

// new Date(epochMs).toISOString(): "2026-11-02T10:00:00.000Z".
std::string isoFromEpochMs(double epochMs);

// --- Is the bike's clock fit to copy into the VCU RTC? --------------------------------------

struct PiClockEvidence {
  double systemEpochMs = 0;
  std::optional<double> gpsEpochSeconds;
  std::optional<double> gpsAgeMs;  // monotonic age, never a wall-clock difference
};
struct PiClockVerdict {
  bool trustworthy = false;
  std::string iso;
  double offsetFromGpsSeconds = 0;     // when trustworthy
  std::vector<std::string> reasons;    // when not
};
PiClockVerdict checkPiClock(const PiClockEvidence& evidence);

// --- The 0x120 RTC-sync frame ---------------------------------------------------------------

inline constexpr std::uint32_t kRtcSyncCanId = 0x120;
// Packs a UTC instant into the 8-byte 0x120 frame; nullopt if the year is outside 2000..2255.
std::optional<std::array<std::uint8_t, 8>> buildRtcSyncFrame(double epochMs);

struct RtcFields {
  int hour, minute, second, day, weekday, month, year;
};
// Unpacks a 0x120 clock frame, or nullopt if it is not one of ours. For checks both directions.
std::optional<RtcFields> decodeRtcSyncFrame(std::span<const std::uint8_t> frame);

// --- OBD Mode 04: clear stored trouble codes ------------------------------------------------

inline constexpr std::uint32_t kObdFunctionalRequestId = 0x7df;
std::array<std::uint8_t, 8> buildClearDtcsFrame();
// Could this frame be an answer to OUR Mode 04, rather than someone else's traffic? Permissive
// about which ECU answered, strict about what it said.
bool isClearDtcsReply(std::span<const std::uint8_t> frame);

struct ClearDtcsReply {
  enum class Kind : std::uint8_t { kCleared, kRefused, kUnrecognised };
  Kind kind = Kind::kUnrecognised;
  int negativeResponseCode = 0;  // kRefused
  std::string description;       // kRefused
  std::string reason;            // kUnrecognised
};
ClearDtcsReply decodeClearDtcsReply(std::span<const std::uint8_t> frame);

}  // namespace emcool::vcu
