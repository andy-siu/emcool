#pragma once

// One service-write exchange on the bus: open the 10 81 session, read-before (the
// compare-and-swap), unlock with SecurityAccess, write 2E and read back; and the service actions —
// read/set the service stamp (31 FC), clear DTCs (OBD Mode 04), reset both micros (11 02), sync
// the clock (0x120). Coroutines on emcool::async, like kwp_client: one exchange in flight at a
// time, 300 ms reply window, 10 ms pace after each.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "emcool/async/task.hpp"
#include "emcool/vcu/catalog.hpp"
#include "emcool/vcu/codec.hpp"
#include "emcool/vcu/kwp_client.hpp"
#include "emcool/vcu/service_actions.hpp"
#include "emcool/vcu/snapshot.hpp"
#include "emcool/vcu/write_codec.hpp"
#include "emcool/vcu/write_targets.hpp"

namespace emcool::vcu {

// How far a parameter write may be from the last SecurityAccess on a micro before it must wait —
// asking a still-unlocked micro for a seed returns invalidKey and spends one of ~3 attempts.
inline constexpr double kSecurityCooldownMs = 4000;

// Per-micro last-authenticated marks, owned by the write runner so a cooldown survives across
// write() calls but resets cleanly between test runs.
class SecurityCooldown {
 public:
  double remainingMs(Micro micro, double nowMs) const;
  void mark(Micro micro, double nowMs);
  void clear() { marks_.clear(); }

 private:
  std::map<Micro, double> marks_;
};

enum class WriteStage : std::uint8_t {
  kSession,
  kReadBefore,
  kSecuritySeed,
  kSecurityKey,
  kWrite,
  kReadBack,
  kRoutine,
};
const char* writeStageName(WriteStage stage);

struct ServiceWriteOutcome {
  enum class Status : std::uint8_t { kWritten, kReadBackMismatch, kStalePrecondition, kRefused, kFailed };
  Status status = Status::kFailed;
  ParameterWritePlan plan;
  double readBack = 0;              // kWritten, kReadBackMismatch
  std::string rawHex;              // kWritten, kReadBackMismatch
  double actual = 0;               // kStalePrecondition
  WriteStage stage = WriteStage::kSession;
  int negativeResponseCode = 0;    // kRefused
  std::string description;         // kRefused
  std::string reason;              // kFailed
};

struct BatchWriteOutcome {
  enum class Status : std::uint8_t { kRan, kFailed };
  Status status = Status::kFailed;
  std::vector<ServiceWriteOutcome> results;  // kRan
  WriteStage stage = WriteStage::kSession;   // kFailed
  std::string reason;                        // kFailed
};

struct StampRead {
  bool ok = false;
  ServiceStamp stamp;
  std::string reason;
};

struct ServicePointOutcome {
  enum class Status : std::uint8_t { kStarted, kRefused, kFailed };
  Status status = Status::kFailed;
  std::optional<ServiceStamp> before;  // kStarted
  std::optional<ServiceStamp> after;   // kStarted
  std::string stage;                   // session | security-seed | security-key | routine
  int negativeResponseCode = 0;        // kRefused
  std::string description;             // kRefused
  std::string reason;                  // kFailed
};

struct ClearDtcsOutcome {
  enum class Status : std::uint8_t { kCleared, kRefused, kFailed };
  Status status = Status::kFailed;
  int negativeResponseCode = 0;  // kRefused
  std::string description;       // kRefused
  std::string reason;            // kFailed
};

struct ResetVcuOutcome {
  enum class Status : std::uint8_t { kReset, kRefused, kFailed };
  Status status = Status::kFailed;
  std::string note;              // kReset
  std::string micro;             // kRefused, kFailed
  std::string stage;             // kFailed: "session" | "reset"
  int negativeResponseCode = 0;  // kRefused
  std::string description;       // kRefused
  std::string reason;            // kFailed
};

struct SyncClockOutcome {
  bool sent = false;
  std::string hex;     // sent
  std::string reason;  // !sent
};

// One write session on the bus. The runner holds it (frames route to handleFrame; the watchdog
// aborts it) and co_awaits exactly one of the run* coroutines.
class WriteSession : public std::enable_shared_from_this<WriteSession> {
 public:
  WriteSession(VcuSend send, const ParameterNames& names, SecurityCooldown& cooldown)
      : send_(std::move(send)), names_(names), cooldown_(cooldown) {}

  bool handleFrame(std::uint32_t id, std::span<const std::uint8_t> data);
  void abort(std::string reason);

  async::Task<ServiceWriteOutcome> parameterWrite(ParameterWritePlan plan, const TableTypeReport* report, bool present);
  async::Task<BatchWriteOutcome> parameterWrites(std::vector<ParameterWritePlan> plans, const TableTypeReport* report,
                                                 bool present);
  async::Task<StampRead> readServiceStamp(double nowMs);
  async::Task<ServicePointOutcome> setServicePoint(double nowMs);
  async::Task<ClearDtcsOutcome> clearStoredDtcs();
  async::Task<ResetVcuOutcome> resetVcu();

 private:
  struct ExchangeResult {
    enum class Kind : std::uint8_t { kReply, kTimeout, kNotSent } kind = Kind::kTimeout;
    VcuFrame frame;
    std::string reason;
    static ExchangeResult reply(VcuFrame frame);
    static ExchangeResult timeout();
    static ExchangeResult notSent(std::string reason);
  };
  struct Pending {
    std::function<void(ExchangeResult)> settle;
    async::TimerId timer;
  };
  struct ParameterRead {
    bool ok = false;
    double value = 0;
    std::string rawHex;
    std::string reason;
  };

  double nowMs() const;
  async::Task<ExchangeResult> exchangeRaw(std::uint32_t requestCanId, std::optional<std::uint32_t> responseCanId,
                                          std::array<std::uint8_t, 8> frame, bool isObd);
  async::Task<ExchangeResult> exchangeRead(Micro micro, VcuRequest request);
  async::Task<ExchangeResult> exchangeWrite(Micro micro, VcuWriteRequest request);
  async::Task<bool> openSession(Micro micro);
  async::Task<ParameterRead> readParameterValue(Micro micro, int index);
  async::Task<ServiceWriteOutcome> writeAndReadBack(Micro micro, ParameterWritePlan plan, const TableTypeReport* report,
                                                    bool present);
  async::Task<StampRead> readStampInSession(double nowMs);

  struct UnlockResult {
    enum class Kind : std::uint8_t { kUnlocked, kRefused, kFailed } kind = Kind::kFailed;
    WriteStage stage = WriteStage::kSecuritySeed;
    int negativeResponseCode = 0;
    std::string description;
    std::string reason;
  };
  async::Task<UnlockResult> unlock(Micro micro);

  VcuSend send_;
  const ParameterNames& names_;
  SecurityCooldown& cooldown_;
  std::optional<Pending> pending_;
  std::optional<std::uint32_t> pendingResponseCanId_;
  bool pendingIsObd_ = false;
  std::optional<Micro> unlockedMicro_;
  bool stopped_ = false;
  std::string stoppedReason_;
};

// Synchronous: broadcasts the clock on 0x120. No session, no reply.
SyncClockOutcome syncBikeClock(const VcuSend& send, double nowMs);

std::string describeExchangeFailure(bool timeout, const std::string& reason);

}  // namespace emcool::vcu
