#pragma once

// A copy of the log for a network console: while a client is logged in, every log line also goes
// into its sink's buffer, for the console's task to send. Whoever logs only ever copies into the
// buffer, never touches the network: the CAN decode task must not wait on a slow client, and Wi-Fi's
// and lwIP's own tasks log too. A full buffer drops the line and counts it.
//
// The telnet console and the SSH console each have one; the serial port gets every line as before.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

namespace emcool {

class LogSink {
 public:
  // A sink holding up to `bytes` of log between drains. Null without the memory, or with every sink
  // already handed out. Made once and kept.
  static LogSink* create(std::size_t bytes);

  // A client logged in: from here its lines are kept, starting from nothing.
  void start();
  void stop();
  // What has been kept since the last drain, and a note of any dropped, appended to `out`. From the
  // console's own task only.
  void drain(std::string& out);

  // From the log hook.
  void offer(const char* line, std::size_t bytes);

 private:
  explicit LogSink(StreamBufferHandle_t buffer) : buffer_(buffer) {}

  StreamBufferHandle_t buffer_;
  std::mutex mutex_;  // a stream buffer takes one writer at a time
  std::atomic<bool> on_{false};
  std::atomic<std::uint32_t> dropped_{0};
};

}  // namespace emcool
