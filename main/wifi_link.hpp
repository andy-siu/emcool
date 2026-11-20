#pragma once

// Wi-Fi, off until WIFI_SSID and ADMIN_PASSWORD are both set, and while WIFI_ENABLED is 0: the bike
// joins that network as a client (reconnecting on its own whenever it drops), and serves, both behind
// ADMIN_PASSWORD:
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
  std::atomic<bool> sshClient{false};
  std::atomic<std::uint32_t> sshLogins{0};
  std::atomic<std::uint32_t> sshRefusals{0};
  std::atomic<std::uint32_t> pagesServed{0};
  std::atomic<std::uint32_t> pagesRefused{0};
  std::atomic<std::uint64_t> bytesDownloaded{0};
};

// Where the link is, as the dashboard's header shows it (the reference's wifi_link_state codes):
// off, not joined (and retrying), joining (associating, or waiting for an address), joined.
enum class WifiLinkState : std::uint8_t { kOff = 0, kNotJoined = 1, kJoining = 2, kJoined = 3 };

// The ride log the web page serves: its own download, so a page download and a Bluetooth one can
// run at once. Null without a card. Starts nothing while WIFI_ENABLED is 0, but keeps what
// switchWifi() needs to start it later.
bool startWifi(const settings::Settings& settings, settings::SettingsStorage& storage,
               settings::RuntimeSettings& running, ride_log::DownloadCard* card, WifiCounters& counters);

// WIFI_ENABLED, while running: off stops the radio's Wi-Fi side (which closes every SSH, telnet and
// page connection), on starts it again with the boot's network, or for the first time if it was off
// at boot. False if it could not; for RuntimeSettings::setWifiSwitch.
bool switchWifi(bool on);
// Whether Wi-Fi is up now: started, and not switched off since.
bool wifiRunning();
WifiLinkState wifiLinkState();

// The `ifconfig` command's report: the network joined (its SSID and signal), and the
// address, network, netmask, gateway and DNS server it handed out, or why there are none, then the
// hostname and the interface's MAC address. One line each. With Wi-Fi off, says so, and the MAC.
std::string describeNetwork();

}  // namespace emcool
