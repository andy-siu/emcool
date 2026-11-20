#include "ssh_console.hpp"

// wolfSSL first: its settings decide what wolfSSH's headers declare.
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "can_console.hpp"
#include "card_files.hpp"
#include "console_io.hpp"
#include "dashboard_console.hpp"
#include "emcool/files/sftp_server.hpp"
#include "emcool/log.hpp"
#include "emcool/settings/line_editor.hpp"
#include "emcool/settings/login_limiter.hpp"
#include "emcool/settings/remote_console.hpp"
#include "emcool/text/help.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "firmware_update.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tee.hpp"
#include "lwip/sockets.h"

// ------------------------------------------------------------------------------------- memory

// wolfSSL's and wolfSSH's allocations (XMALLOC_USER, set in the top-level CMakeLists.txt): the key
// exchange's numbers, each connection's buffers. PSRAM first, so the internal RAM Wi-Fi and
// Bluetooth need is left alone; internal RAM if there's none. These are the only allocators the
// two libraries have, so they're in place before anything of wolfSSL runs.
extern "C" void* XMALLOC(size_t size, void*, int) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}
extern "C" void XFREE(void* pointer, void*, int) { heap_caps_free(pointer); }
extern "C" void* XREALLOC(void* pointer, size_t size, void*, int) {
  return heap_caps_realloc_prefer(pointer, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

namespace emcool {

namespace {

constexpr const char* kTag = "ssh";
constexpr std::uint16_t kPort = 22;
// Core 0 with Wi-Fi, NimBLE and the ride-log task; core 1 is the CAN decode task's. The key exchange
// is a few hundred milliseconds of curve25519 and Ed25519 at this priority, below the ride log's.
constexpr BaseType_t kCore = 0;
constexpr UBaseType_t kPriority = 2;
// The key exchange's big numbers live on the heap (WOLFSSL_SMALL_STACK), but the console's commands
// run on this stack too.
constexpr std::uint32_t kStackBytes = 12 * 1024;
constexpr std::size_t kLogBufferBytes = 4096;
// wolfSSH's defaults are 128 KB of window and 32 KB packets per channel, all of it allocated up front.
constexpr word32 kWindowBytes = 4096;
constexpr word32 kMaxPacketBytes = 4096;
constexpr std::int64_t kIdleMs = 30 * 60 * 1000;
constexpr std::int64_t kLoginMs = 60 * 1000;
// The NVS key the host key is kept under (settings storage's namespace), as DER.
constexpr std::string_view kHostKeyStorageKey = "ssh_host_key";
// A card command prints at most this much before the console sends it.
constexpr std::size_t kCardOutputBytes = 64 * 1024;

std::int64_t nowMs() { return esp_timer_get_time() / 1000; }

// Overwrites a secret before its memory is let go, in a way the compiler can't drop.
void wipe(void* data, std::size_t length) {
  volatile auto* bytes = static_cast<volatile std::uint8_t*>(data);
  for (std::size_t at = 0; at < length; at++) bytes[at] = 0;
}

struct Server {
  settings::SettingsStorage* storage = nullptr;
  settings::RuntimeSettings* running = nullptr;
  WifiCounters* counters = nullptr;
  WOLFSSH_CTX* ctx = nullptr;
  LogSink* log = nullptr;
  settings::LoginLimiter limiter;
  std::string fingerprint;
  std::mutex fingerprintMutex;
} server;

// ----------------------------------------------------------------------------------- host key

std::string base64(const std::uint8_t* data, std::size_t length, bool pad) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t at = 0; at < length; at += 3) {
    const std::uint32_t bits = (static_cast<std::uint32_t>(data[at]) << 16) |
                               (at + 1 < length ? static_cast<std::uint32_t>(data[at + 1]) << 8 : 0) |
                               (at + 2 < length ? static_cast<std::uint32_t>(data[at + 2]) : 0);
    out += kAlphabet[(bits >> 18) & 63];
    out += kAlphabet[(bits >> 12) & 63];
    if (at + 1 < length) out += kAlphabet[(bits >> 6) & 63];
    else if (pad) out += '=';
    if (at + 2 < length) out += kAlphabet[bits & 63];
    else if (pad) out += '=';
  }
  return out;
}

std::string hex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char byte : bytes) {
    out += kDigits[(static_cast<unsigned char>(byte) >> 4) & 15];
    out += kDigits[static_cast<unsigned char>(byte) & 15];
  }
  return out;
}

std::optional<std::string> unhex(std::string_view text) {
  if (text.size() % 2 != 0) return std::nullopt;
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t at = 0; at < text.size(); at += 2) {
    const int high = digit(text[at]);
    const int low = digit(text[at + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out += static_cast<char>((high << 4) | low);
  }
  return out;
}

// The SSH fingerprint of an Ed25519 public key: SHA-256 over its wire form, unpadded base64.
std::string fingerprintOf(const std::uint8_t (&publicKey)[ED25519_PUB_KEY_SIZE]) {
  static constexpr char kType[] = "ssh-ed25519";
  std::vector<std::uint8_t> blob;
  const auto put = [&](const std::uint8_t* data, std::uint32_t length) {
    for (int shift = 24; shift >= 0; shift -= 8) blob.push_back(static_cast<std::uint8_t>(length >> shift));
    blob.insert(blob.end(), data, data + length);
  };
  put(reinterpret_cast<const std::uint8_t*>(kType), sizeof kType - 1);
  put(publicKey, ED25519_PUB_KEY_SIZE);
  std::uint8_t digest[WC_SHA256_DIGEST_SIZE] = {};
  wc_Sha256Hash(blob.data(), static_cast<word32>(blob.size()), digest);
  return "SHA256:" + base64(digest, sizeof digest, false);
}

// The host key, as DER: the one in NVS, or a new one, saved. Its fingerprint goes to `fingerprint`.
std::optional<std::string> hostKey(settings::SettingsStorage& storage, std::string& fingerprint) {
  ed25519_key key;
  if (wc_ed25519_init(&key) != 0) return std::nullopt;
  std::string der;
  bool made = false;
  if (const std::optional<std::string> stored = storage.read(kHostKeyStorageKey)) {
    if (const std::optional<std::string> bytes = unhex(*stored)) {
      word32 index = 0;
      if (wc_Ed25519PrivateKeyDecode(reinterpret_cast<const byte*>(bytes->data()), &index, &key,
                                     static_cast<word32>(bytes->size())) == 0) {
        der = *bytes;
      }
    }
    if (der.empty()) logWarning(kTag, "the stored host key doesn't decode; making a new one");
  }
  if (der.empty()) {
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) {
      wc_ed25519_free(&key);
      return std::nullopt;
    }
    const int result = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
    wc_FreeRng(&rng);
    if (result != 0) {
      logError(kTag, "making the host key failed (%d)", result);
      wc_ed25519_free(&key);
      return std::nullopt;
    }
    byte buffer[128];
    const int length = wc_Ed25519KeyToDer(&key, buffer, sizeof buffer);
    if (length <= 0) {
      logError(kTag, "encoding the host key failed (%d)", length);
      wc_ed25519_free(&key);
      return std::nullopt;
    }
    der.assign(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(length));
    wipe(buffer, sizeof buffer);
    made = true;
  }
  std::uint8_t publicKey[ED25519_PUB_KEY_SIZE] = {};
  word32 publicLength = sizeof publicKey;
  wc_ed25519_export_public(&key, publicKey, &publicLength);
  wc_ed25519_free(&key);
  fingerprint = fingerprintOf(publicKey);
  if (made) {
    if (!storage.write(kHostKeyStorageKey, hex(der))) {
      logError(kTag, "couldn't save the new host key to NVS; a client will see a new one at every boot");
    } else {
      logWarning(kTag, "made a new host key: %s. Check it against what ssh shows the first time.", fingerprint.c_str());
    }
  }
  return der;
}

// --------------------------------------------------------------------------------- the session

// The user name is ignored: there is one user. Only password auth is offered.
int userAuth(byte type, WS_UserAuthData* data, void*) {
  if (type != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_FAILURE;
  const std::string_view attempt(reinterpret_cast<const char*>(data->sf.password.password),
                                 data->sf.password.passwordSz);
  switch (server.limiter.check(attempt, server.running->boot().adminPassword, nowMs())) {
    case settings::LoginLimiter::Verdict::kAccepted:
      return WOLFSSH_USERAUTH_SUCCESS;
    case settings::LoginLimiter::Verdict::kWrong:
      return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    case settings::LoginLimiter::Verdict::kWrongLast:
    case settings::LoginLimiter::Verdict::kLockedOut:
      // Ends the connection, as the client has no more tries.
      return WOLFSSH_USERAUTH_REJECTED;
  }
  return WOLFSSH_USERAUTH_FAILURE;
}

int userAuthTypes(WOLFSSH*, void*) { return WOLFSSH_USERAUTH_PASSWORD; }

// The one subsystem served: sftp, which plain scp, sftp and the GUI clients ask for. Any other is
// refused.
int checkSubsystem(WOLFSSH_CHANNEL* channel, void*) {
  const char* name = wolfSSH_ChannelGetSessionCommand(channel);
  return name != nullptr && std::strcmp(name, "sftp") == 0 ? 0 : 1;
}

// The card's files over SFTP (emcool/files/sftp_server.hpp), on the session's channel. It reads
// what has come as soon as there's room for it, and sends its answers as the client takes them, so
// a long download or upload streams without either side's buffers filling.
void runSftp(WOLFSSH* ssh, int socket) {
  if (cardFiles().card == nullptr) {
    logWarning(kTag, "an SFTP session, but there's no card: %s", kNoCardConfigured);
    return;
  }
  auto transfer = std::make_unique<files::SftpServer>(*cardFiles().card, cardFiles().gate, "SSH file transfer");
  if (!transfer->ready()) {
    logError(kTag, "no memory for an SFTP session (free: %u internal, %u PSRAM)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return;
  }
  logInfo(kTag, "an SFTP session started");
  std::int64_t lastHeardMs = nowMs();
  byte buffer[1024];
  while (true) {
    // Out first: the server answers a request only while there's room for its answer.
    bool blocked = false;
    while (!transfer->pendingOutput().empty()) {
      const std::string_view out = transfer->pendingOutput();
      const int sent = wolfSSH_stream_send(ssh, reinterpret_cast<byte*>(const_cast<char*>(out.data())),
                                           static_cast<word32>(std::min<std::size_t>(out.size(), kMaxPacketBytes)));
      if (sent > 0) {
        transfer->sent(static_cast<std::size_t>(sent));
        continue;
      }
      const int error = wolfSSH_get_error(ssh);
      if (sent == WS_WINDOW_FULL || error == WS_WINDOW_FULL || sent == WS_WANT_WRITE || error == WS_WANT_WRITE ||
          sent == WS_REKEYING || error == WS_REKEYING) {
        blocked = true;  // the client's window adjustment, or the socket, has to come first
        break;
      }
      logInfo(kTag, "the SFTP session ended: %s", wolfSSH_ErrorToName(error));
      return;
    }
    if (!transfer->work()) return;
    // Read only while there's room: what's left in wolfSSH's channel buffer holds the window shut,
    // so the client waits.
    const std::size_t room = std::min(sizeof buffer, transfer->inputRoom());
    if (room > 0) {
      const int peeked = wolfSSH_stream_peek(ssh, nullptr, static_cast<word32>(room));
      if (peeked > 0) {
        const int got = wolfSSH_stream_read(ssh, buffer, static_cast<word32>(room));
        if (got > 0) {
          lastHeardMs = nowMs();
          if (!transfer->receive(std::string_view(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(got)))) {
            return;
          }
          continue;
        }
      } else if (peeked < 0 && wolfSSH_get_error(ssh) == WS_EOF) {
        logInfo(kTag, "the SFTP session ended");
        return;
      }
    }
    if (nowMs() - lastHeardMs > kIdleMs) {
      logInfo(kTag, "the SFTP session was idle too long; ending it");
      return;
    }
    // Wait for the client: its requests, its window adjustments, a rekey.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval wait = {};
    wait.tv_usec = (blocked || room == 0 ? 20 : 200) * 1000;
    const int ready = select(socket + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) return;
    if (ready == 0 && !blocked) continue;
    word32 channel = 0;
    const int result = wolfSSH_worker(ssh, &channel);
    const int error = wolfSSH_get_error(ssh);
    if (result == WS_SUCCESS || result == WS_CHAN_RXD || error == WS_CHAN_RXD || result == WS_WANT_READ ||
        error == WS_WANT_READ || result == WS_WANT_WRITE || error == WS_WANT_WRITE || result == WS_REKEYING ||
        error == WS_REKEYING || error == WS_WINDOW_FULL) {
      continue;
    }
    if (result == WS_CHANNEL_CLOSED || error == WS_CHANNEL_CLOSED || error == WS_EOF) {
      logInfo(kTag, "the SFTP session ended");
    } else {
      logInfo(kTag, "the SFTP session ended: %s", wolfSSH_ErrorToName(error));
    }
    return;
  }
}

// One SSH session's console: what commands print goes out on its channel, as it's printed.
class SshSession final : public ConsoleIo {
 public:
  SshSession(WOLFSSH* ssh, int socket) : ssh_(ssh), socket_(socket) {}

  const char* name() const override { return "SSH console"; }

  void write(std::string_view text) override {
    for (const char character : text) {
      if (character == '\n' && terminal_) out_ += '\r';
      out_ += character;
    }
    if (out_.size() >= 1024) flush();
  }
  void flush() override {
    std::size_t sent = 0;
    while (sent < out_.size() && alive_) {
      const int result =
          wolfSSH_stream_send(ssh_, reinterpret_cast<byte*>(out_.data() + sent), static_cast<word32>(out_.size() - sent));
      if (result > 0) {
        sent += static_cast<std::size_t>(result);
        continue;
      }
      const int error = wolfSSH_get_error(ssh_);
      if (result == WS_WANT_WRITE || error == WS_WANT_WRITE || result == WS_WINDOW_FULL || error == WS_WINDOW_FULL ||
          result == WS_REKEYING || error == WS_REKEYING) {
        // The client hasn't room for more yet: let wolfSSH read its window adjustment.
        if (!pump(50)) break;
        continue;
      }
      alive_ = false;
    }
    out_.clear();
  }

  void watchKeys() override {
    flush();
    pending_.clear();
    pump(30);
    pending_.clear();
    watching_ = true;
  }
  bool interrupted() override {
    flush();
    if (!alive_) return true;
    pump(0);
    if (!pending_.empty()) {
      pending_.clear();
      return true;
    }
    return !alive_;
  }
  void endWatch() override { watching_ = false; }

  // The session: the shell's prompt and lines, or one exec command.
  void run();

 private:
  // Waits up to `waitMs` for the socket, then lets wolfSSH take what has come (data, a window
  // adjustment, a rekey). What the client typed lands in pending_. False once the session is over.
  bool pump(int waitMs);
  void runLine(const std::string& line);
  void runCommand(int argumentCount, char** arguments);
  void prompt() {
    write("emcool# ");
    flush();
  }

  WOLFSSH* ssh_;
  int socket_;
  bool terminal_ = false;
  bool alive_ = true;
  bool watching_ = false;
  bool close_ = false;
  std::string out_;
  std::string pending_;
  std::int64_t lastHeardMs_ = nowMs();
};

bool SshSession::pump(int waitMs) {
  if (!alive_) return false;
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(socket_, &readable);
  timeval wait = {};
  wait.tv_sec = waitMs / 1000;
  wait.tv_usec = (waitMs % 1000) * 1000;
  const int ready = select(socket_ + 1, &readable, nullptr, nullptr, &wait);
  if (ready < 0) {
    alive_ = false;
    return false;
  }
  if (ready == 0) return true;
  word32 channel = 0;
  const int result = wolfSSH_worker(ssh_, &channel);
  const int error = wolfSSH_get_error(ssh_);
  if (result == WS_CHAN_RXD || error == WS_CHAN_RXD) {
    byte buffer[256];
    const int got = wolfSSH_ChannelIdRead(ssh_, channel, buffer, sizeof buffer);
    if (got > 0) {
      pending_.append(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(got));
      lastHeardMs_ = nowMs();
    }
    return true;
  }
  if (result == WS_SUCCESS || result == WS_WANT_READ || error == WS_WANT_READ || result == WS_WANT_WRITE ||
      error == WS_WANT_WRITE || result == WS_REKEYING || error == WS_REKEYING) {
    return true;
  }
  if (result == WS_CHANNEL_CLOSED || error == WS_CHANNEL_CLOSED || error == WS_EOF) {
    alive_ = false;
    return false;
  }
  alive_ = false;
  return false;
}

// The full console: every command the USB console runs.
std::unique_ptr<files::CardShell> cardShell;

void SshSession::runCommand(int argumentCount, char** arguments) {
  const std::string_view command = arguments[0];
  const auto flushText = [this](std::string& piece) {
    write(piece);
    flush();
  };
  if (isCanCommand(command)) {
    runCanCommand(*this, argumentCount, arguments);
    return;
  }
  if (command == "update") {
    runUpdateCommand(argumentCount, arguments, cardFiles().card, flushText);
    return;
  }
  if (files::CardShell::handles(command)) {
    if (cardFiles().card == nullptr) {
      write(kNoCardConfigured);
      return;
    }
    if (!cardShell) {
      cardShell = std::make_unique<files::CardShell>(*cardFiles().card, cardFiles().gate, "SSH console", kCardOutputBytes);
    }
    cardShell->run(argumentCount, arguments, flushText);
    return;
  }
  if (command == "help") {
    static constexpr text::HelpLine kHelp[] = {
        {"", "Settings:"},
        {"show", "list every setting and its value"},
        {"set NAME VALUE", "save a setting; quote a value with spaces"},
        {"unset NAME", "put a setting back to its default"},
        {"status", "print the status report now"},
        {"status on | off", "turn the periodic status report on or off"},
        {"status every SECONDS", "how often it prints (5-3600)"},
        {"", "Most settings take effect after `restart`. What each one does is in the USB\n"
             "console's `help`, and in docs/setup.md."},
        {"", ""},
        {"", "The bike:"},
        {"ifconfig", "the Wi-Fi network, address, host key and MAC address"},
        {"update FILE", "install firmware from the card: update /emcool.bin"},
        {"update status", "which firmware runs now, and which boots next"},
        {"restart", "restart, so saved settings take effect"},
        {"exit", "leave this console (also quit, logout, Ctrl-D)"},
        {"", ""},
    };
    std::string text;
    text::appendHelp(text, kHelp);
    text::appendHelp(text, canCommandHelp());
    text += "\n";
    text::appendHelp(text, files::CardShell::help());
    write(text);
    return;
  }
  if (command == "ifconfig") {
    write(describeNetwork());
    return;
  }
  if (command == "restart") {
    const std::vector<std::string> blockers = restartBlockersNow();
    if (!blockers.empty()) {
      std::string why = "not restarting:";
      for (const std::string& blocker : blockers) why += " " + blocker + ";";
      why.back() = '\n';
      write(why);
      return;
    }
    write("restarting\n");
    flush();
    logWarning(kTag, "restart asked for over the SSH console");
    restartAfter(500);
    return;
  }
  if (command == "show" || command == "set" || command == "unset" || command == "status") {
    if (command == "set" || command == "unset") {
      logInfo(kTag, "over the SSH console: %s %s", arguments[0], argumentCount > 1 ? arguments[1] : "");
    }
    std::string output;
    settings::runSettingsCommand(*server.storage, *server.running, argumentCount, arguments, output);
    write(output);
    return;
  }
  write("no command called " + std::string(command) + "; `help` lists them\n");
}

void SshSession::runLine(const std::string& line) {
  constexpr std::size_t kMaxArguments = 12;
  std::vector<std::string> words = settings::splitWords(line);
  if (words.empty()) return;
  if (words[0] == "exit" || words[0] == "quit" || words[0] == "logout") {
    close_ = true;
    return;
  }
  if (words.size() > kMaxArguments) {
    write("too many words; a command takes at most " + std::to_string(kMaxArguments) + "\n");
    return;
  }
  std::array<char*, kMaxArguments + 1> arguments = {};
  for (std::size_t at = 0; at < words.size(); at++) arguments[at] = words[at].data();
  runCommand(static_cast<int>(words.size()), arguments.data());
  flush();
}

void SshSession::run() {
  const WS_SessionType type = wolfSSH_GetSessionType(ssh_);
  if (type == WOLFSSH_SESSION_EXEC) {
    const char* command = wolfSSH_GetSessionCommand(ssh_);
    logInfo(kTag, "running `%s` for an SSH client", command != nullptr ? command : "");
    if (command != nullptr) runLine(command);
    flush();
    return;
  }
  // A shell: with a terminal (ssh asks for a pty when it has one), the server echoes and edits.
  terminal_ = true;
  settings::LineEditor editor(terminal_);
  write("emcool SSH console. `help` lists the commands, `exit` leaves. The log follows this console.\n");
  prompt();
  server.log->start();
  while (alive_ && !close_) {
    pump(100);
    if (!pending_.empty()) {
      const settings::LineEditor::Result result = editor.feed(pending_);
      pending_.clear();
      if (!result.echo.empty()) {
        out_ += result.echo;
        flush();
      }
      for (const std::string& line : result.lines) {
        // The log stays out of the way while a command runs and prints.
        runLine(line);
        if (close_ || !alive_) break;
        prompt();
      }
      if (result.interrupted && result.lines.empty()) prompt();
      if (result.endOfInput) break;
    }
    std::string log;
    server.log->drain(log);
    if (!log.empty()) {
      // On its own lines, then the prompt and what was typed so far again.
      write("\r\x1b[K");
      write(log);
      write("emcool# ");
      out_ += editor.pending();
      flush();
    }
    if (nowMs() - lastHeardMs_ > kIdleMs) {
      write("\nidle too long; bye\n");
      flush();
      break;
    }
  }
  server.log->stop();
  if (alive_) {
    write("bye\n");
    flush();
  }
}

// ---------------------------------------------------------------------------------- the server

void serveClient(int client) {
  timeval timeout = {};
  timeout.tv_sec = 5;
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  const int on = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  // The handshake blocks on the socket, so it has a limit of its own: a client that connects and
  // says nothing is let go.
  timeval loginTimeout = {};
  loginTimeout.tv_sec = kLoginMs / 1000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &loginTimeout, sizeof loginTimeout);

  WOLFSSH* ssh = wolfSSH_new(server.ctx);
  if (ssh == nullptr) {
    logError(kTag, "no memory for an SSH session (free: %u internal, %u PSRAM)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return;
  }
  wolfSSH_set_fd(ssh, client);
  server.limiter.connected();
  const int accepted = wolfSSH_accept(ssh);
  if (accepted != WS_SUCCESS) {
    const int error = wolfSSH_get_error(ssh);
    server.counters->sshRefusals.fetch_add(1, std::memory_order_relaxed);
    if (error == WS_USER_AUTH_E) {
      logWarning(kTag, "an SSH login was refused");
    } else {
      logInfo(kTag, "an SSH connection ended before login: %s", wolfSSH_ErrorToName(error));
    }
    wolfSSH_free(ssh);
    return;
  }
  // From here the session polls with select(), so reads mustn't block.
  timeval none = {};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
  const int flags = lwip_fcntl(client, F_GETFL, 0);
  lwip_fcntl(client, F_SETFL, flags | O_NONBLOCK);
  server.counters->sshLogins.fetch_add(1, std::memory_order_relaxed);
  logInfo(kTag, "logged in over SSH");
  if (wolfSSH_GetSessionType(ssh) == WOLFSSH_SESSION_SUBSYSTEM) {
    // scp, sftp and the GUI clients: the card's files.
    runSftp(ssh, client);
  } else {
    if (cardShell) cardShell->reset();
    SshSession session(ssh, client);
    session.run();
  }
  wolfSSH_stream_exit(ssh, 0);
  wolfSSH_shutdown(ssh);
  wolfSSH_free(ssh);
}

void serverTask(void*) {
  const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  const int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      listen(listener, 1) != 0) {
    logError(kTag, "SSH could not listen on port %u (errno %d)", kPort, errno);
    vTaskDelete(nullptr);
    return;
  }
  while (true) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    server.counters->sshClient.store(true, std::memory_order_relaxed);
    serveClient(client);
    shutdown(client, SHUT_RDWR);
    close(client);
    server.counters->sshClient.store(false, std::memory_order_relaxed);
  }
}

}  // namespace

std::string sshHostKeyFingerprint() {
  {
    const std::lock_guard<std::mutex> lock(server.fingerprintMutex);
    if (!server.fingerprint.empty()) return server.fingerprint;
  }
  return kNoHostKey;
}

bool startSshConsole(settings::SettingsStorage& storage, settings::RuntimeSettings& running, WifiCounters& counters) {
  server.storage = &storage;
  server.running = &running;
  server.counters = &counters;
  if (wolfSSH_Init() != WS_SUCCESS) {
    logError(kTag, "wolfSSH didn't start; SSH stays off");
    return false;
  }
  std::string fingerprint;
  std::optional<std::string> key = hostKey(storage, fingerprint);
  if (!key) {
    logError(kTag, "no host key; SSH stays off");
    return false;
  }
  {
    const std::lock_guard<std::mutex> lock(server.fingerprintMutex);
    server.fingerprint = fingerprint;
  }
  server.ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
  if (server.ctx == nullptr) {
    logError(kTag, "no memory for SSH; it stays off");
    return false;
  }
  const int loaded = wolfSSH_CTX_UsePrivateKey_buffer(server.ctx, reinterpret_cast<const byte*>(key->data()),
                                                      static_cast<word32>(key->size()), WOLFSSH_FORMAT_ASN1);
  wipe(key->data(), key->size());
  if (loaded != WS_SUCCESS) {
    logError(kTag, "wolfSSH refused the host key (%d); SSH stays off", loaded);
    return false;
  }
  wolfSSH_CTX_SetAlgoListKex(server.ctx, "curve25519-sha256,ecdh-sha2-nistp256");
  wolfSSH_CTX_SetAlgoListKey(server.ctx, "ssh-ed25519");
  wolfSSH_CTX_SetAlgoListCipher(server.ctx, "aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes128-ctr");
  wolfSSH_CTX_SetAlgoListMac(server.ctx, "hmac-sha2-256,hmac-sha2-512");
  wolfSSH_CTX_SetWindowPacketSize(server.ctx, kWindowBytes, kMaxPacketBytes);
  wolfSSH_SetUserAuth(server.ctx, userAuth);
  wolfSSH_SetUserAuthTypes(server.ctx, userAuthTypes);
  wolfSSH_CTX_SetChannelReqSubsysCb(server.ctx, checkSubsystem);
  wolfSSH_CTX_SetBanner(server.ctx, "emcool: the SSH console. The password is ADMIN_PASSWORD; any user name will do.\n");
  server.log = LogSink::create(kLogBufferBytes);
  if (server.log == nullptr) {
    logError(kTag, "no memory for the SSH console's log buffer; SSH stays off");
    return false;
  }
  if (xTaskCreatePinnedToCore(serverTask, "ssh", kStackBytes, nullptr, kPriority, nullptr, kCore) != pdPASS) {
    logError(kTag, "could not create the SSH task");
    return false;
  }
  logInfo(kTag, "listening on port %u; host key %s", kPort, fingerprint.c_str());
  return true;
}

}  // namespace emcool
