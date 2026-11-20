#include "emcool/settings/line_editor.hpp"

namespace emcool::settings {

LineEditor::Result LineEditor::feed(std::string_view bytes) {
  Result result;
  for (const char signedByte : bytes) {
    const auto byte = static_cast<unsigned char>(signedByte);
    // ESC [ … final, ESC O x: a cursor key, Home, Delete. Stepped over whole.
    if (escape_ == Escape::kStarted) {
      escape_ = byte == '[' ? Escape::kCsi : byte == 'O' ? Escape::kSs3 : Escape::kNone;
      continue;
    }
    if (escape_ == Escape::kSs3) {
      escape_ = Escape::kNone;
      continue;
    }
    if (escape_ == Escape::kCsi) {
      if (byte >= 0x40 && byte <= 0x7e) escape_ = Escape::kNone;
      continue;
    }
    if (byte == 0x1b) {
      escape_ = Escape::kStarted;
      continue;
    }
    // CR LF and CR NUL are one line end.
    if ((byte == '\n' || byte == '\0') && lastWasCr_) {
      lastWasCr_ = false;
      continue;
    }
    lastWasCr_ = byte == '\r';
    if (byte == '\r' || byte == '\n') {
      if (terminal_) result.echo += "\r\n";
      if (overlong_) {
        result.lines.emplace_back();
        result.echo += "that line was longer than " + std::to_string(kMaxLineBytes) + " characters; ignored\r\n";
      } else {
        result.lines.push_back(std::move(pending_));
      }
      pending_.clear();
      overlong_ = false;
      hidden_ = false;
      continue;
    }
    if (byte == 0x03) {  // Ctrl-C: drop the line
      if (terminal_) result.echo += "^C\r\n";
      pending_.clear();
      overlong_ = false;
      hidden_ = false;
      result.interrupted = true;
      continue;
    }
    if (byte == 0x04) {  // Ctrl-D: the end, on an empty line
      if (pending_.empty()) {
        result.endOfInput = true;
        return result;
      }
      continue;
    }
    if (byte == 0x15) {  // Ctrl-U: clear the line
      if (terminal_ && !hidden_) {
        for (std::size_t at = 0; at < pending_.size(); at++) result.echo += "\b \b";
      }
      pending_.clear();
      overlong_ = false;
      continue;
    }
    if (byte == 0x7f || byte == 0x08) {
      if (!pending_.empty()) {
        // A UTF-8 character's continuation bytes go with it.
        while (!pending_.empty() && (static_cast<unsigned char>(pending_.back()) & 0xc0) == 0x80) pending_.pop_back();
        if (!pending_.empty()) pending_.pop_back();
        if (terminal_ && !hidden_) result.echo += "\b \b";
      }
      continue;
    }
    if (byte < 0x20) continue;  // other control characters
    if (pending_.size() >= kMaxLineBytes) {
      overlong_ = true;
      continue;
    }
    pending_ += static_cast<char>(byte);
    if (terminal_ && !hidden_) result.echo += static_cast<char>(byte);
  }
  return result;
}

}  // namespace emcool::settings
