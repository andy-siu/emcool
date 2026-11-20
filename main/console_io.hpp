#pragma once

// Where a console command prints and how it notices a keypress, so the same command runs on the USB
// console (stdout and the serial port) and the SSH console (its channel). A command that runs until
// a key is pressed (can-dump, can-send -r) calls watchKeys() before it starts waiting, interrupted()
// as it goes, and endWatch() when it stops; KeyWatch does the three.

#include <cstdarg>
#include <string>
#include <string_view>

namespace emcool {

class ConsoleIo {
 public:
  virtual ~ConsoleIo() = default;
  // "USB console", "SSH console": for the log, and for the card's "another console has it".
  virtual const char* name() const = 0;
  virtual void write(std::string_view text) = 0;
  // Sends what write() has held back. A console may send at once, or batch small writes.
  virtual void flush() {}
  // From here, anything typed (or the client going away) makes interrupted() true. What was already
  // waiting is thrown away: the Enter that ran the command mustn't stop it at once.
  virtual void watchKeys() = 0;
  virtual bool interrupted() = 0;
  virtual void endWatch() {}

  // printf into write().
  void print(const char* format, ...) __attribute__((format(printf, 2, 3)));
};

class KeyWatch {
 public:
  explicit KeyWatch(ConsoleIo& io) : io_(io) { io_.watchKeys(); }
  ~KeyWatch() { io_.endWatch(); }
  KeyWatch(const KeyWatch&) = delete;
  KeyWatch& operator=(const KeyWatch&) = delete;
  bool pressed() { return io_.interrupted(); }

 private:
  ConsoleIo& io_;
};

inline void ConsoleIo::print(const char* format, ...) {
  char small[256];
  va_list arguments;
  va_start(arguments, format);
  va_list copy;
  va_copy(copy, arguments);
  const int length = vsnprintf(small, sizeof small, format, arguments);
  va_end(arguments);
  if (length < 0) {
    va_end(copy);
    return;
  }
  if (static_cast<std::size_t>(length) < sizeof small) {
    va_end(copy);
    write(std::string_view(small, static_cast<std::size_t>(length)));
    return;
  }
  std::string large(static_cast<std::size_t>(length) + 1, '\0');
  vsnprintf(large.data(), large.size(), format, copy);
  va_end(copy);
  large.resize(static_cast<std::size_t>(length));
  write(large);
}

}  // namespace emcool
