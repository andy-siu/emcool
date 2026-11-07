#pragma once

// /settings for the phone dashboard: cool-eva's SettingsReply (src/settings/view.ts), filled from
// what this boot runs with. The dashboard's riding views read it — Cooling labels the coolant
// probes and the fan by whether they are installed, the header raises a fault, Charge links to
// the charging permission — so a bike with no /settings would show them all as unconfirmed.
//
// Read-only on purpose. cool-eva saves a change through PATCH /settings and applies it at an
// explicit restart; on emcool the serial console is that path, so the reply says no change is
// pending and the PATCH is refused (ble_server/endpoints.hpp). Fields describing Pi-only things
// (data directories, a ride-log public key, the motorcycle hub) carry the values that make the
// dashboard show them as not in use.

#include <string>
#include <string_view>

#include "emcool/settings/settings.hpp"

namespace emcool::settings {

struct DashboardReplyContext {
  // The firmware's version, as `idf.py` stamps it.
  std::string_view version;
  // Differs on every boot, so the dashboard can tell a restart from a reconnect: the ride log's
  // session id serves.
  std::string_view processGeneration;
  // Stable for this bike across boots: what the dashboard keys its per-bike drafts and notices on.
  std::string_view instanceId;
  // Whether a microSD card is configured, for the "recording" readiness line.
  bool rideLogConfigured;
  // Names the Bluetooth key without revealing it (UUID-shaped, derived from the key), so the
  // dashboard says a key is saved and files its copies under it; empty for none.
  std::string_view bluetoothCodeId = {};
};

std::string dashboardSettingsReply(const Settings& settings, const DashboardReplyContext& context);

// A JSON string literal for `text`: quotes, backslashes and control characters escaped.
std::string jsonString(std::string_view text);

}  // namespace emcool::settings
