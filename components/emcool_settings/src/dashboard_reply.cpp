#include "emcool/settings/dashboard_reply.hpp"

#include <cstdio>

namespace emcool::settings {

std::string jsonString(std::string_view text) {
  std::string out = "\"";
  for (const char character : text) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(character));
          out += escaped;
        } else {
          out += character;
        }
    }
  }
  return out + "\"";
}

namespace {

const char* jsonBoolean(bool value) { return value ? "true" : "false"; }

// The reference's PublicSettings. Saved and active are the same object: nothing is ever pending here.
std::string publicSettings(const Settings& settings, std::string_view bluetoothCodeId) {
  std::string out = "{";
  // Not ported yet, so honestly off: the dashboard then shows them as not installed.
  out += "\"coolantProbesEnabled\":false,\"radiatorFanEnabled\":false";
  out += ",\"customBmsConfigurationFlashed\":";
  out += jsonBoolean(settings.customBmsConfig);
  out += ",\"automaticDcChargingEnabled\":";
  out += jsonBoolean(settings.chargeAutoEnabled);
  out += ",\"phoneHotspotSsid\":";
  out += jsonString(settings.wifiSsid);
  out += ",\"rideLogDirectory\":\"/sdcard/RIDES\",\"vcuParameterDirectory\":\"/sdcard/vcu-params\"";
  // No hub link, no ride-log key: emcool has neither.
  out += ",\"hubAddress\":null,\"discoverHubOnce\":false,\"recordingKeyConfigured\":false,\"bluetoothCodeId\":";
  out += bluetoothCodeId.empty() ? std::string("null") : jsonString(bluetoothCodeId);
  out += "}";
  return out;
}

std::string canReadiness(const Settings& settings) {
  if (!settings.obdEnabled) return "Listen-only: OBD_ENABLED is 0, so nothing is transmitted";
  if (!settings.serviceModeEnabled) return "Active node: OBD polling on; service mode off (SERVICE_MODE_ENABLED 0)";
  if (!settings.serviceWriteEnabled) return "Active node: OBD and service-mode reads on; writes off";
  return "Active node: OBD, service-mode reads and writes on";
}

}  // namespace

std::string dashboardSettingsReply(const Settings& settings, const DashboardReplyContext& context) {
  const std::string values = publicSettings(settings, context.bluetoothCodeId);
  std::string out = "{\"ok\":true,\"source\":\"fresh\",\"version\":";
  out += jsonString(context.version);
  out += ",\"recordingFingerprint\":null,\"processGeneration\":";
  out += jsonString(context.processGeneration);
  out += ",\"instanceId\":";
  out += jsonString(context.instanceId);
  out += ",\"revision\":0,\"saved\":" + values + ",\"active\":" + values;
  out += ",\"pending\":[],\"fault\":null,\"notices\":[],\"readiness\":{";
  out += "\"coolant\":\"Not on emcool yet\",\"fan\":\"Not on emcool yet\",\"recording\":";
  out += jsonString(context.rideLogConfigured ? "Recording to the microSD card, in plain text"
                                              : "Off: no microSD pins configured");
  out += ",\"bluetooth\":\"On: this phone is connected with the key\",\"hub\":\"Not on emcool\",\"can\":";
  out += jsonString(canReadiness(settings));
  out += "},\"restart\":{\"state\":\"idle\",\"message\":\"\"}";
  out += ",\"locations\":{\"root\":\"microSD card\",\"wifi\":\"\",\"captures\":\"\"}}";
  return out;
}

}  // namespace emcool::settings
