#include "log_tee.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>

#include "esp_log_write.h"

namespace emcool {

namespace {

// The telnet console's and the SSH console's.
constexpr std::size_t kMaxSinks = 2;

std::array<std::atomic<LogSink*>, kMaxSinks> sinks{};
std::mutex createMutex;
vprintf_like_t serialPrint = nullptr;

// Every log line goes to the serial port as before, and to every sink that's on.
int teeLog(const char* format, va_list arguments) {
  va_list copy;
  va_copy(copy, arguments);
  const int written = serialPrint(format, arguments);
  char line[384];
  int length = -1;
  for (const std::atomic<LogSink*>& slot : sinks) {
    LogSink* sink = slot.load(std::memory_order_acquire);
    if (sink == nullptr) continue;
    if (length < 0) length = vsnprintf(line, sizeof line, format, copy);
    if (length <= 0) break;
    sink->offer(line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof line - 1));
  }
  va_end(copy);
  return written;
}

}  // namespace

LogSink* LogSink::create(std::size_t bytes) {
  const std::lock_guard<std::mutex> lock(createMutex);
  for (std::atomic<LogSink*>& slot : sinks) {
    if (slot.load(std::memory_order_relaxed) != nullptr) continue;
    const StreamBufferHandle_t buffer = xStreamBufferCreate(bytes, 1);
    if (buffer == nullptr) return nullptr;
    auto* sink = new (std::nothrow) LogSink(buffer);
    if (sink == nullptr) {
      vStreamBufferDelete(buffer);
      return nullptr;
    }
    slot.store(sink, std::memory_order_release);
    if (serialPrint == nullptr) serialPrint = esp_log_set_vprintf(teeLog);
    return sink;
  }
  return nullptr;
}

void LogSink::start() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    xStreamBufferReset(buffer_);
  }
  dropped_.store(0, std::memory_order_relaxed);
  on_.store(true, std::memory_order_release);
}

void LogSink::stop() { on_.store(false, std::memory_order_release); }

void LogSink::offer(const char* line, std::size_t bytes) {
  if (!on_.load(std::memory_order_acquire)) return;
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock() || xStreamBufferSend(buffer_, line, bytes, 0) != bytes) {
    dropped_.fetch_add(static_cast<std::uint32_t>(bytes), std::memory_order_relaxed);
  }
}

void LogSink::drain(std::string& out) {
  char chunk[256];
  while (true) {
    const std::size_t got = xStreamBufferReceive(buffer_, chunk, sizeof chunk, 0);
    if (got == 0) break;
    out.append(chunk, got);
  }
  const std::uint32_t dropped = dropped_.exchange(0, std::memory_order_relaxed);
  if (dropped > 0) out += "[" + std::to_string(dropped) + " bytes of log dropped: the network was too slow]\n";
}

}  // namespace emcool
