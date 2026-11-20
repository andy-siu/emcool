#pragma once

// The SSH console on port 22, while Wi-Fi is on and ADMIN_PASSWORD is set: `ssh admin@<bike>`, the
// password, then every command the USB console has — settings, restart, firmware updates from the
// card, the card's commands, and the manual CAN commands (can-send, can-dump, psu, …) — with the log
// following, as on the serial port. `ssh admin@<bike> COMMAND` runs one command and leaves.
//
// wolfSSH: curve25519 key exchange, an Ed25519 host key, AES-GCM or AES-CTR with HMAC-SHA2. The host
// key is made on the first start and kept in NVS, so the client's known_hosts recognises the bike
// from then on; `ifconfig` prints its fingerprint, to check against what ssh shows the first time.
// One client at a time; three wrong passwords end the connection, and every attempt waits 5 s after
// three wrong in a row. The user name is ignored. Port forwarding, agent forwarding, SCP and SFTP
// are all off.

#include <string>

#include "emcool/settings/settings.hpp"
#include "wifi_link.hpp"

namespace emcool {

// Starts the listener's task. False (and logged) if the host key can't be loaded or made.
bool startSshConsole(settings::SettingsStorage& storage, settings::RuntimeSettings& running, WifiCounters& counters);

// "SHA256:…", as ssh prints it, or kNoHostKey before the first start has made one.
inline constexpr const char* kNoHostKey = "none yet (made the first time SSH starts)";
std::string sshHostKeyFingerprint();

}  // namespace emcool
