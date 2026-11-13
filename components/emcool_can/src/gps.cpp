#include "emcool/can/gps.hpp"

#include "emcool/log.hpp"

namespace emcool::can {

namespace {

constexpr std::uint8_t kLatitudeSubFrame = 0x00;
constexpr std::uint8_t kLongitudeSubFrame = 0x01;
constexpr std::uint8_t kSummarySubFrame = 0xfe;

// 2026-01-01T00:00:00Z. Catches the GPS week-number rollover (1980/1999) and a zeroed date
// (2000). Deliberately not "today": replaying older captures must keep their gps_epoch_s.
constexpr double kGpsUtcFloorEpochSeconds = 1767225600;

constexpr double kSuppressedFractionToComplain = 0.2;
constexpr std::uint32_t kMinimumSample = 40;

bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int year, unsigned month) {
  constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeapYear(year) ? 29 : kDays[month - 1];
}

// Days from 1970-01-01 to a proleptic-Gregorian date (Howard Hinnant's days_from_civil).
std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

}  // namespace

void GpsMessageDecoder::decode(FrameBytes frame, DecodedValues& out) {
  if (frame.size() < kHubFrameSize || frame[0] != kGpsMessageType) {
    return;
  }

  if (frame[1] == kLatitudeSubFrame) {
    latitudeDeciMilliminutes_ = ((frame[4] >> 2) & 63u) | (unsigned{frame[5]} << 6);
    latitudeMinutes_ = frame[6] & 63u;
    latitudeDegrees_ = ((frame[6] >> 6) & 3u) | ((frame[7] & 63u) << 2);
    latitudeSign_ = ((frame[7] >> 6) & 1) != 0 ? -1 : 1;
    haveLatitude_ = true;
    out.push("gps_course_deg", frame[2] | ((frame[3] & 1u) << 8));
    out.push("gps_speed_kmh", ((frame[3] >> 1) & 0x7fu) | ((frame[4] & 3u) << 7));
    return;
  }

  if (frame[1] == kLongitudeSubFrame) {
    longitudeDeciMilliminutes_ = ((frame[4] >> 2) & 63u) | (unsigned{frame[5]} << 6);
    longitudeMinutes_ = frame[6] & 63u;
    longitudeDegrees_ = ((frame[6] >> 6) & 3u) | ((frame[7] & 63u) << 2);
    longitudeSign_ = ((frame[7] >> 6) & 1) != 0 ? -1 : 1;
    haveLongitude_ = true;
    const unsigned raw = ((frame[2] >> 2) & 63u) | (unsigned{frame[3]} << 6) | ((frame[4] & 3u) << 14);
    const int altitude = raw >= 0x8000 ? static_cast<int>(raw) - 0x10000 : static_cast<int>(raw);
    fix_ = frame[2] & 3u;
    out.push("gps_fix", fix_);
    out.push("gps_altitude_m", altitude);
    return;
  }

  if (frame[1] == kSummarySubFrame) {
    // Same operation order as the TypeScript so the doubles come out bit-identical.
    const double latitude =
        latitudeSign_ * (latitudeDegrees_ + (latitudeMinutes_ + latitudeDeciMilliminutes_ / 10000.0) / 60.0);
    const double longitude =
        longitudeSign_ * (longitudeDegrees_ + (longitudeMinutes_ + longitudeDeciMilliminutes_ / 10000.0) / 60.0);
    const unsigned satellites = (frame[7] >> 3) & 31u;
    out.push("gps_satellites", satellites);
    const bool bothAxesFresh = haveLatitude_ && haveLongitude_ && fix_ != 0;
    if (bothAxesFresh && (latitude != 0 || longitude != 0)) {
      out.push("gps_lat", latitude);
      out.push("gps_lon", longitude);
      emittedFixes++;
    } else if (fix_ != 0 && !(haveLatitude_ && haveLongitude_)) {
      suppressedFixes++;
    }
    haveLatitude_ = false;
    haveLongitude_ = false;
    double epochSeconds = 0;
    if (decodeUtc(frame, satellites, epochSeconds)) {
      out.push("gps_epoch_s", epochSeconds);
    }
  }
}

bool GpsMessageDecoder::decodeUtc(FrameBytes frame, unsigned satellites, double& epochSeconds) const {
  if (fix_ == 0 || satellites < 4) {
    return false;
  }
  const unsigned milliseconds = frame[2] | ((frame[3] & 3u) << 8);
  const unsigned seconds = (frame[3] >> 2) & 63u;
  const unsigned minutes = frame[4] & 63u;
  const unsigned hours = ((frame[4] >> 6) & 3u) | ((frame[5] & 7u) << 2);
  const unsigned day = (frame[5] >> 3) & 31u;
  const unsigned month = frame[6] & 15u;
  const int year = 2000 + static_cast<int>(((frame[6] >> 4) & 15u) | ((frame[7] & 7u) << 4));
  const bool inRange = month >= 1 && month <= 12 && day >= 1 && day <= 31 && hours < 24 && minutes < 60 &&
                       seconds < 60 && milliseconds < 1000;
  if (!inRange) {
    return false;
  }
  // Reject a date Date.UTC would roll over (31 April → 1 May); with every other field in range
  // that is exactly a day past the end of its month.
  if (static_cast<int>(day) > daysInMonth(year, month)) {
    return false;
  }
  const std::int64_t epochMilliseconds =
      ((((daysFromCivil(year, month, day) * 24 + hours) * 60 + minutes) * 60 + seconds) * 1000) + milliseconds;
  const double candidate = static_cast<double>(epochMilliseconds) / 1000.0;
  if (candidate < kGpsUtcFloorEpochSeconds) {
    return false;
  }
  epochSeconds = candidate;
  return true;
}

void SuppressedFixWatcher::check() {
  const std::uint32_t total = decoder_.suppressedFixes + decoder_.emittedFixes;
  if (total < kMinimumSample) {
    return;
  }
  const double fraction = static_cast<double>(decoder_.suppressedFixes) / total;
  if (fraction < kSuppressedFractionToComplain) {
    return;
  }
  complaints_++;
  if (complaints_ < nextComplaintAt_) {
    return;
  }
  nextComplaintAt_ *= 2;
  logWarning("gps",
             "(%s): %.0f %% of fixes withheld (%u of %u) — the hub is sending time but not both coordinate "
             "sub-frames, so position is being withheld rather than blended from a stale half",
             transport_, fraction * 100, static_cast<unsigned>(decoder_.suppressedFixes),
             static_cast<unsigned>(total));
}

}  // namespace emcool::can
