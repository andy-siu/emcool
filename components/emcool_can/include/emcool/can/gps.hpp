#pragma once

// The hub's GPS multiplex (message type 26): one fix spans three sub-frames — 0x00 latitude
// plus course/speed, 0x01 longitude plus fix/altitude, 0xFE satellites and UTC — so this is
// the one decoder that keeps state between frames. Ported from cool-eva src/gps/decode.ts
// and src/gps/fix-watch.ts; the bit layouts and their evidence are documented there.

#include <cstdint>

#include "emcool/can/frame.hpp"

namespace emcool::can {

inline constexpr std::uint8_t kGpsMessageType = 26;
inline constexpr std::size_t kHubFrameSize = 8;

class GpsMessageDecoder {
 public:
  void decode(FrameBytes frame, DecodedValues& out);

  // A 0xFE sub-frame with a fix but without both coordinate halves since the last one:
  // position is withheld rather than blended from a stale half.
  std::uint32_t suppressedFixes = 0;
  std::uint32_t emittedFixes = 0;

 private:
  bool decodeUtc(FrameBytes frame, unsigned satellites, double& epochSeconds) const;

  int latitudeSign_ = 1;
  unsigned latitudeDegrees_ = 0;
  unsigned latitudeMinutes_ = 0;
  unsigned latitudeDeciMilliminutes_ = 0;
  bool haveLatitude_ = false;

  int longitudeSign_ = 1;
  unsigned longitudeDegrees_ = 0;
  unsigned longitudeMinutes_ = 0;
  unsigned longitudeDeciMilliminutes_ = 0;
  bool haveLongitude_ = false;

  unsigned fix_ = 0;
};

// Complains, with exponential back-off, when a fifth or more of fixes are being withheld.
class SuppressedFixWatcher {
 public:
  SuppressedFixWatcher(const GpsMessageDecoder& decoder, const char* transport)
      : decoder_(decoder), transport_(transport) {}

  void check();

 private:
  const GpsMessageDecoder& decoder_;
  const char* transport_;
  std::uint32_t complaints_ = 0;
  std::uint32_t nextComplaintAt_ = 1;
};

}  // namespace emcool::can
