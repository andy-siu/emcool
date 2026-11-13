#pragma once

// The card commands' view of the bike (emcool/files/card_shell.hpp): the mount the ride-log task
// owns, the ride it is recording, and the card's free space. One gate for every console, so only
// one changes the card at a time.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "emcool/files/card_shell.hpp"
#include "emcool/ride_log/log_writer.hpp"
#include "ride_log_task.hpp"

namespace emcool {

class BikeCard final : public files::Card {
 public:
  BikeCard(CardAccess& access, ride_log::LogWriter& writer, const RideLogTaskCounters& counters)
      : access_(access), writer_(writer), counters_(counters) {}

  const std::string& mountPoint() const override { return mountPoint_; }
  std::mutex& lock() override { return access_.mountLock(); }
  files::CardState state() override;
  files::CardFacts facts() override;
  std::optional<files::FreeSpace> freeSpace() override;
  void changed(bool rideLog) override;

 private:
  CardAccess& access_;
  ride_log::LogWriter& writer_;
  const RideLogTaskCounters& counters_;
  const std::string mountPoint_ = kSdMountPoint;
};

// What every console shares: the card and its gate. Null until setCardFiles(), and without a card.
struct CardFiles {
  files::Card* card = nullptr;
  files::CardShellGate gate;
};

CardFiles& cardFiles();
void setCardFiles(files::Card* card);

// What a console prints when the ride log never started (no microSD pins, safe mode, or a startup
// step that failed before it), so there is no card to work on.
inline constexpr const char* kNoCardConfigured =
    "no card: the ride log isn't running, so neither is the card (the boot log says why)\n";

}  // namespace emcool
