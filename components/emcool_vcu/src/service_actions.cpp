#include "emcool/vcu/service_actions.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include "emcool/text/js.hpp"
#include "emcool/vcu/codec.hpp"

namespace emcool::vcu {

namespace {

constexpr int kObdClearDtcsMode = 0x04;

// cool-eva's `0x${n.toString(16)}` — lower-case, no padding. (Its toHex, by contrast, is upper.)
std::string hex(int value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%x", value);
  return text;
}
constexpr std::uint8_t kRtcSyncHeader0 = 0x94;
constexpr std::uint8_t kRtcSyncHeader1 = 0xff;

// Days since 1970-01-01 for a civil (y,m,d), Howard Hinnant's algorithm. Exact, no timezone.
std::int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yy = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
  y = yy + (m <= 2);
}

double utcMs(int y, int mo, int d) {
  return static_cast<double>(daysFromCivil(y, mo, d)) * 86400000.0;
}

// new Date(epochMs).toISOString(). Assumes epochMs within the representable civil range.
std::string toIso(double epochMs) {
  std::int64_t ms = static_cast<std::int64_t>(std::floor(epochMs));
  std::int64_t day = ms / 86400000;
  std::int64_t rem = ms - day * 86400000;
  if (rem < 0) {
    rem += 86400000;
    day -= 1;
  }
  int y, mo, d;
  civilFromDays(day, y, mo, d);
  const int msOfDay = static_cast<int>(rem);
  const int hour = msOfDay / 3600000;
  const int minute = (msOfDay / 60000) % 60;
  const int second = (msOfDay / 1000) % 60;
  const int milli = msOfDay % 1000;
  char text[40];
  std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, hour, minute, second, milli);
  return text;
}

const double kServiceDateEpochMs = utcMs(2000, 1, 1);
const double kEarliestCredibleServiceMs = utcMs(2019, 1, 1);
const double kPlausibleClockFloorMs = utcMs(2026, 8, 16);
const double kPlausibleClockCeilingMs = utcMs(2036, 8, 16);
constexpr double kClockAgreementLimitSeconds = 60;
constexpr double kGpsEvidenceMaxAgeMs = 120000;

std::optional<std::string> describeImplausibleStamp(double dateSeconds, double stampMs, double nowMs) {
  if (dateSeconds == 0) {
    return "reads zero — no service point has ever been set on this bike, or A8 answered with an empty cell";
  }
  if (dateSeconds == 4294967295.0) {
    return "reads 0xFFFFFFFF — an erased EEPROM cell, not a date";
  }
  if (stampMs < kEarliestCredibleServiceMs) {
    return "decodes to " + toIso(stampMs) + ", before this motorcycle existed";
  }
  if (stampMs > nowMs) {
    return "decodes to " + toIso(stampMs) +
           ", which is in the future — either the bike's RTC is wrong, or this bike's clock is";
  }
  return std::nullopt;
}

std::string describeClearRefusal(int code) {
  switch (code) {
    case 0x12:
      return "subFunctionNotSupported — this ECU does not implement mode 04 at all";
    case 0x22:
      return "conditionsNotCorrect — an ECU normally refuses to clear while a fault is still ACTIVE. Fix the fault "
             "first; codes that are still true come straight back anyway";
    case 0x31:
      return "requestOutOfRange";
    case 0x33:
      return "securityAccessDenied — mode 04 is not meant to need authentication, so this is worth a note in "
             "obd-garage/";
    default: {
      char text[32];
      std::snprintf(text, sizeof text, "NRC 0x%02x", code);
      return text;
    }
  }
}

}  // namespace

std::string isoFromEpochMs(double epochMs) { return toIso(epochMs); }

std::span<const ServiceStampIdentifier> serviceStampIdentifiers() {
  static constexpr ServiceStampIdentifier kIdentifiers[] = {
      {"dateLow", 1000, 0x13e8},
      {"dateHigh", 1001, 0x13e9},
      {"odometerLow", 1002, 0x13ea},
      {"odometerHigh", 1003, 0x13eb},
  };
  return kIdentifiers;
}

ServiceStamp interpretServiceStamp(const ServiceStampRaw& raw, double nowMs) {
  // (dateHigh << 16 >>> 0) + dateLow, as unsigned 32-bit math.
  const double dateSeconds =
      static_cast<double>((static_cast<std::uint32_t>(raw.dateHigh) << 16)) + static_cast<double>(raw.dateLow);
  const double odometer =
      static_cast<double>((static_cast<std::uint32_t>(raw.odometerHigh) << 16)) + static_cast<double>(raw.odometerLow);
  const double stampMs = kServiceDateEpochMs + dateSeconds * 1000;
  ServiceStamp stamp;
  stamp.raw = raw;
  stamp.dateSeconds = dateSeconds;
  stamp.dateIso = std::isfinite(stampMs) ? std::optional<std::string>(toIso(stampMs)) : std::nullopt;
  stamp.odometer = odometer;
  stamp.implausible = describeImplausibleStamp(dateSeconds, stampMs, nowMs);
  return stamp;
}

PiClockVerdict checkPiClock(const PiClockEvidence& evidence) {
  PiClockVerdict verdict;
  const bool finite = std::isfinite(evidence.systemEpochMs);
  verdict.iso = finite ? toIso(evidence.systemEpochMs) : "(not a date)";
  std::vector<std::string>& reasons = verdict.reasons;

  if (!finite) {
    reasons.push_back("this bike's clock does not read as a time at all");
  } else if (evidence.systemEpochMs < kPlausibleClockFloorMs) {
    reasons.push_back("this bike's clock reads " + verdict.iso +
                      ", before this code was written — it has no RTC and has not been set since boot");
  } else if (evidence.systemEpochMs > kPlausibleClockCeilingMs) {
    reasons.push_back("this bike's clock reads " + verdict.iso +
                      ", which is implausibly far ahead — a GPS date-decode bug once stamped 49 772 rows of this "
                      "bike's log as the year 2060");
  }

  if (!evidence.gpsEpochSeconds || !evidence.gpsAgeMs) {
    reasons.push_back("no satellite time has arrived, so there is nothing to check this bike's clock against");
  } else if (*evidence.gpsAgeMs > kGpsEvidenceMaxAgeMs) {
    reasons.push_back("the last satellite time arrived " + text::jsNumber(text::jsRound(*evidence.gpsAgeMs / 1000)) +
                      " s ago, too long to vouch for the clock now");
  }

  const double offset =
      !evidence.gpsEpochSeconds ? std::nan("") : *evidence.gpsEpochSeconds - evidence.systemEpochMs / 1000;
  if (std::isfinite(offset) && std::fabs(offset) > kClockAgreementLimitSeconds) {
    reasons.push_back("this bike's clock is " + text::jsToFixed(offset, 0) +
                      " s away from satellite time — the GPS clock steps it past 60 s, so it has not managed to");
  }

  if (!reasons.empty()) {
    verdict.trustworthy = false;
    return verdict;
  }
  verdict.trustworthy = true;
  verdict.offsetFromGpsSeconds = offset;
  return verdict;
}

std::optional<std::array<std::uint8_t, 8>> buildRtcSyncFrame(double epochMs) {
  std::int64_t ms = static_cast<std::int64_t>(std::floor(epochMs));
  std::int64_t day = ms / 86400000;
  std::int64_t rem = ms - day * 86400000;
  if (rem < 0) {
    rem += 86400000;
    day -= 1;
  }
  int y, mo, d;
  civilFromDays(day, y, mo, d);
  const int msOfDay = static_cast<int>(rem);
  const int hour = msOfDay / 3600000;
  const int minute = (msOfDay / 60000) % 60;
  const int second = (msOfDay / 1000) % 60;
  // getUTCDay(): Sunday = 0. day 0 (1970-01-01) was a Thursday = 4.
  const int weekday = static_cast<int>(((day % 7) + 7 + 4) % 7);
  const int year = y - 2000;
  if (year < 0 || year > 255) return std::nullopt;
  std::array<std::uint8_t, 8> frame{};
  frame[0] = kRtcSyncHeader0;
  frame[1] = kRtcSyncHeader1;
  frame[2] = static_cast<std::uint8_t>((hour & 0x1f) | ((minute & 0x07) << 5));
  frame[3] = static_cast<std::uint8_t>(((minute & 0x38) >> 3) | ((second & 0x1f) << 3));
  frame[4] = static_cast<std::uint8_t>(((second & 0x20) >> 5) | (d << 1) | ((weekday & 0x03) << 6));
  frame[5] = static_cast<std::uint8_t>(((weekday & 0x04) >> 2) | ((mo & 0x0f) << 1));
  frame[6] = static_cast<std::uint8_t>(year);
  return frame;
}

std::optional<RtcFields> decodeRtcSyncFrame(std::span<const std::uint8_t> frame) {
  if (frame.size() < 7 || frame[0] != kRtcSyncHeader0 || frame[1] != kRtcSyncHeader1) return std::nullopt;
  RtcFields fields;
  fields.hour = frame[2] & 0x1f;
  fields.minute = ((frame[2] >> 5) & 0x07) | ((frame[3] & 0x07) << 3);
  fields.second = ((frame[3] >> 3) & 0x1f) | ((frame[4] & 0x01) << 5);
  fields.day = (frame[4] >> 1) & 0x1f;
  fields.weekday = ((frame[4] >> 6) & 0x03) | ((frame[5] & 0x01) << 2);
  fields.month = (frame[5] >> 1) & 0x0f;
  fields.year = 2000 + frame[6];
  return fields;
}

std::array<std::uint8_t, 8> buildClearDtcsFrame() {
  std::array<std::uint8_t, 8> frame{};
  frame[0] = 0x01;  // one payload byte
  frame[1] = kObdClearDtcsMode;
  return frame;
}

bool isClearDtcsReply(std::span<const std::uint8_t> frame) {
  if (frame.size() < 2 || (frame[0] >> 4) != 0x0) return false;
  if (frame[1] == kObdClearDtcsMode + 0x40) return true;
  return frame[1] == 0x7f && frame.size() >= 3 && frame[2] == kObdClearDtcsMode;
}

ClearDtcsReply decodeClearDtcsReply(std::span<const std::uint8_t> frame) {
  ClearDtcsReply reply;
  if (frame.size() < 2) {
    reply.reason = "frame shorter than a PCI byte and a service byte";
    return reply;
  }
  const int length = frame[0] & 0x0f;
  if ((frame[0] >> 4) != 0x0 || length == 0) {
    reply.reason = "not a single frame: " + toHex(frame);
    return reply;
  }
  if (frame[1] == 0x7f) {
    if (length < 3 || frame.size() < 4) {
      reply.reason = "negative response without a service and a code";
      return reply;
    }
    if (frame[2] != kObdClearDtcsMode) {
      reply.reason = "a refusal of service " + hex(frame[2]) + ", which we did not send";
      return reply;
    }
    reply.kind = ClearDtcsReply::Kind::kRefused;
    reply.negativeResponseCode = frame[3];
    reply.description = describeClearRefusal(frame[3]);
    return reply;
  }
  if (frame[1] == kObdClearDtcsMode + 0x40) {
    reply.kind = ClearDtcsReply::Kind::kCleared;
    return reply;
  }
  char text[48];
  std::snprintf(text, sizeof text, "reply names service 0x%x, not 0x44", frame[1]);
  reply.reason = text;
  return reply;
}

}  // namespace emcool::vcu
