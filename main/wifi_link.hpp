#pragma once

// Wi-Fi, off until WIFI_SSID and ADMIN_PASSWORD are both set: the bike joins that network as a
// client (reconnecting on its own whenever it drops), and serves, both behind ADMIN_PASSWORD:
//
//   port 23  the console: `nc <bike> 23`, the password, then the USB console's commands, with the
//            log (status reports and all) following it, as on the serial port
//   port 80  the ride log: http://<bike>/ lists every boot and part; /dl is the dashboard's
//            download, the same body as over Bluetooth, only faster
//
// Plain TCP and plain HTTP: the password and everything after it cross the network in the clear, so
// anyone on that network can watch them. It runs beside Bluetooth, sharing the radio.

#include <atomic>
#include <cstdint>
#include <string>

#include "emcool/ride_log/download.hpp"
#include "emcool/settings/settings.hpp"

namespace emcool {

struct WifiCounters {
  std::atomic<bool> connected{false};
  // The address, in network byte order, while connected.
  std::atomic<std::uint32_t> address{0};
  std::atomic<std::uint32_t> reconnects{0};
  std::atomic<bool> consoleClient{false};
  std::atomic<std::uint32_t> consoleLogins{0};
  std::atomic<std::uint32_t> consoleRefusals{0};
  std::atomic<std::uint32_t> pagesServed{0};
  std::atomic<std::uint32_t> pagesRefused{0};
  std::atomic<std::uint64_t> bytesDownloaded{0};
};

// The ride log the web page serves: its own download, so a page download and a Bluetooth one can
// run at once. Null without a card.
bool startWifi(const settings::Settings& settings, settings::SettingsStorage& storage,
               settings::RuntimeSettings& running, ride_log::DownloadCard* card, WifiCounters& counters);

// The `network` command's report: the network joined, its signal, and the address, netmask,
// gateway and DNS server the network handed out, or why there are none. One line each.
std::string describeNetwork();

}  // namespace emcool
