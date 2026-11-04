#pragma once

// What never reaches the ride log on the microSD card. The card is plain text, so anything that
// says where the bike has been stays off it. These signals still update live state, so the clock
// and the phone can use them — they are just never written down.

#include <string_view>

namespace emcool::signals {

inline constexpr std::string_view kExcludedFromRideLog[] = {
    "gps_lat",
    "gps_lon",
    // Heading and altitude against time are enough to rebuild a route's shape.
    "gps_course_deg",
    "gps_altitude_m",
    "waypoint_lat",
    "waypoint_lon",
};

bool isExcludedFromRideLog(std::string_view key);

}  // namespace emcool::signals
