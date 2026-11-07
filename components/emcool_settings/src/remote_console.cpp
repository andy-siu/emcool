#include "emcool/settings/remote_console.hpp"

#include <array>
#include <vector>

namespace emcool::settings {

namespace {

constexpr unsigned char kIac = 255;
constexpr unsigned char kSb = 250;
constexpr unsigned char kSe = 240;
constexpr unsigned char kWill = 251;
constexpr unsigned char kWont = 252;
constexpr unsigned char kDont = 254;
constexpr unsigned char kEcho = 1;
constexpr std::size_t kMaxArguments = 8;

}  // namespace

std::vector<std::string> splitWords(std::string_view line) {
  std::vector<std::string> words;
  std::string word;
  bool inWord = false;
  bool quoted = false;
  for (std::size_t at = 0; at < line.size(); at++) {
    const char character = line[at];
    if (character == '\\' && at + 1 < line.size()) {
      word += line[++at];
      inWord = true;
    } else if (character == '"') {
      quoted = !quoted;
      inWord = true;
    } else if (character == ' ' && !quoted) {
      if (inWord) words.push_back(std::move(word));
      word.clear();
      inWord = false;
    } else {
      word += character;
      inWord = true;
    }
  }
  if (inWord) words.push_back(std::move(word));
  return words;
}

bool constantTimeEquals(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t at = 0; at < left.size(); at++) {
    difference |= static_cast<unsigned char>(left[at] ^ right[at]);
  }
  return difference == 0;
}

std::optional<std::string> basicAuthPassword(std::string_view header) {
  constexpr std::string_view kScheme = "basic ";
  if (header.size() < kScheme.size()) return std::nullopt;
  for (std::size_t at = 0; at < kScheme.size(); at++) {
    const char character = header[at];
    if ((character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character) != kScheme[at]) return std::nullopt;
  }
  std::string_view encoded = header.substr(kScheme.size());
  while (!encoded.empty() && encoded.front() == ' ') encoded.remove_prefix(1);
  while (!encoded.empty() && encoded.back() == ' ') encoded.remove_suffix(1);
  if (encoded.empty() || encoded.size() % 4 != 0) return std::nullopt;
  const auto value = [](char character) -> int {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
  };
  std::string decoded;
  for (std::size_t at = 0; at < encoded.size(); at += 4) {
    int sextets[4];
    int padding = 0;
    for (int index = 0; index < 4; index++) {
      const char character = encoded[at + index];
      if (character == '=' && at + 4 == encoded.size() && index >= 2) {
        sextets[index] = 0;
        padding++;
        continue;
      }
      if (padding > 0) return std::nullopt;  // data after padding
      sextets[index] = value(character);
      if (sextets[index] < 0) return std::nullopt;
    }
    const std::uint32_t bits = (static_cast<std::uint32_t>(sextets[0]) << 18) | (static_cast<std::uint32_t>(sextets[1]) << 12) |
                               (static_cast<std::uint32_t>(sextets[2]) << 6) | static_cast<std::uint32_t>(sextets[3]);
    decoded += static_cast<char>((bits >> 16) & 0xff);
    if (padding < 2) decoded += static_cast<char>((bits >> 8) & 0xff);
    if (padding < 1) decoded += static_cast<char>(bits & 0xff);
  }
  const std::size_t colon = decoded.find(':');
  if (colon == std::string::npos) return std::nullopt;
  return decoded.substr(colon + 1);
}

std::string withCrLf(std::string_view text) {
  std::string out;
  out.reserve(text.size() + text.size() / 16);
  for (const char character : text) {
    if (character == '\n') out += '\r';
    out += character;
  }
  return out;
}

std::string RemoteConsole::connected(std::int64_t) {
  authenticated_ = false;
  attemptsThisConnection_ = 0;
  pending_.clear();
  overlong_ = false;
  lastWasCr_ = false;
  telnet_ = Telnet::kData;
  // Asks a telnet client not to echo the password (WILL ECHO); nc shows the bytes and ignores them.
  return std::string("emcool console over Wi-Fi. Everything here crosses the network in plain text.\r\npassword: ") +
         static_cast<char>(kIac) + static_cast<char>(kWill) + static_cast<char>(kEcho);
}

std::string RemoteConsole::prompt() const { return "emcool> "; }

std::string RemoteConsole::receive(std::span<const char> bytes, std::int64_t nowMs, bool& close) {
  std::string out;
  close = false;
  for (const char signedByte : bytes) {
    const auto byte = static_cast<unsigned char>(signedByte);
    // Telnet negotiation, stepped over: IAC WILL/WONT/DO/DONT <option>, IAC SB … IAC SE, and
    // IAC <command>. IAC IAC is a literal 255, which no command needs, so it is dropped too.
    switch (telnet_) {
      case Telnet::kData:
        if (byte == kIac) {
          telnet_ = Telnet::kCommand;
          continue;
        }
        break;
      case Telnet::kCommand:
        telnet_ = byte == kSb ? Telnet::kSubnegotiation : byte >= kWill && byte <= kDont ? Telnet::kOption : Telnet::kData;
        continue;
      case Telnet::kOption:
        telnet_ = Telnet::kData;
        continue;
      case Telnet::kSubnegotiation:
        if (byte == kIac) telnet_ = Telnet::kSubnegotiationIac;
        continue;
      case Telnet::kSubnegotiationIac:
        telnet_ = byte == kSe ? Telnet::kData : Telnet::kSubnegotiation;
        continue;
    }
    if (byte == '\n' && lastWasCr_) {
      lastWasCr_ = false;
      continue;
    }
    lastWasCr_ = byte == '\r';
    if (byte == '\r' || byte == '\n') {
      const std::string text = std::move(pending_);
      pending_.clear();
      if (overlong_) {
        overlong_ = false;
        out += "that line was longer than " + std::to_string(kMaxLineBytes) + " characters; ignored\r\n";
        out += authenticated_ ? prompt() : "password: ";
      } else {
        out += line(text, nowMs, close);
      }
      if (close) return out;
      continue;
    }
    if (byte == 0x7f || byte == 0x08) {
      if (!pending_.empty()) pending_.pop_back();
      continue;
    }
    if (byte < 0x20) continue;  // other control characters, a stray NUL after CR
    if (pending_.size() >= kMaxLineBytes) {
      overlong_ = true;
      continue;
    }
    pending_ += static_cast<char>(byte);
  }
  return out;
}

std::string RemoteConsole::line(std::string_view text, std::int64_t nowMs, bool& close) {
  if (!authenticated_) return login(text, nowMs, close);
  std::vector<std::string> words = splitWords(text);
  if (words.empty()) return prompt();
  if (words[0] == "exit" || words[0] == "quit" || words[0] == "logout") {
    close = true;
    return "bye\r\n";
  }
  std::string output;
  if (words.size() > kMaxArguments) {
    output = "too many words; a command takes at most " + std::to_string(kMaxArguments) + "\n";
  } else {
    std::array<char*, kMaxArguments + 1> arguments = {};
    for (std::size_t at = 0; at < words.size(); at++) arguments[at] = words[at].data();
    run_(static_cast<int>(words.size()), arguments.data(), output);
  }
  // The USB console's output ends lines with LF; a network terminal wants CR LF.
  return withCrLf(output) + prompt();
}

std::string RemoteConsole::login(std::string_view attempt, std::int64_t nowMs, bool& close) {
  if (nowMs < lockedUntilMs_) {
    close = true;
    return "\r\ntoo many wrong passwords; try again in a few seconds\r\n";
  }
  if (constantTimeEquals(attempt, password_)) {
    authenticated_ = true;
    failuresInARow_ = 0;
    // WONT ECHO: the client echoes again from here.
    return std::string() + static_cast<char>(kIac) + static_cast<char>(kWont) + static_cast<char>(kEcho) +
           "\r\nlogged in. `help` lists the commands, `exit` leaves. The log follows this console.\r\n" + prompt();
  }
  failuresInARow_++;
  attemptsThisConnection_++;
  if (failuresInARow_ >= kFreeAttempts) lockedUntilMs_ = nowMs + kLockoutMs;
  if (attemptsThisConnection_ >= kAttemptsPerConnection) {
    close = true;
    return "\r\nwrong password\r\n";
  }
  return "\r\nwrong password\r\npassword: ";
}

}  // namespace emcool::settings
