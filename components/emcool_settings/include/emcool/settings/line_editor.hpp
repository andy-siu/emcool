#pragma once

// The SSH console's line handling: bytes typed into a terminal in, what to echo back out, and each
// line once Return ends it. An SSH client with a terminal sends every keystroke as it is typed and
// shows only what the server echoes, so the server does the editing: printable characters echo,
// Backspace rubs one out, Ctrl-U clears the line, Ctrl-C drops it, and the arrow keys' escape
// sequences are swallowed rather than inserted. Without a terminal (`ssh admin@bike show`, or input
// piped in), nothing is echoed and lines end at LF as they would in a file.
//
// Platform-free, so the host checks run it; main/ssh_console.cpp feeds it an SSH channel.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emcool::settings {

class LineEditor {
 public:
  // A line longer than this is refused whole; a command needs far less, a secret 128 characters.
  static constexpr std::size_t kMaxLineBytes = 512;

  explicit LineEditor(bool terminal) : terminal_(terminal) {}

  struct Result {
    std::string echo;                // to send back to the client now
    std::vector<std::string> lines;  // complete lines, in order
    bool interrupted = false;        // Ctrl-C or Ctrl-D on an empty line
    bool endOfInput = false;         // Ctrl-D on an empty line
  };
  Result feed(std::string_view bytes);
  // What has been typed on the current line so far.
  const std::string& pending() const { return pending_; }
  // Hides what is typed (a password, a secret) until the next line ends.
  void hideNextLine() { hidden_ = true; }

 private:
  bool terminal_;
  std::string pending_;
  bool overlong_ = false;
  bool hidden_ = false;
  bool lastWasCr_ = false;
  enum class Escape : unsigned char { kNone, kStarted, kCsi, kSs3 };
  Escape escape_ = Escape::kNone;
};

}  // namespace emcool::settings
