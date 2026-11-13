#include "card_files.hpp"

#include "sd_card.hpp"

namespace emcool {

files::CardState BikeCard::state() {
  const ride_log::DownloadCard::State state = access_.state();
  return {state.mounted, state.mountGeneration};
}

files::CardFacts BikeCard::facts() {
  files::CardFacts facts;
  if (const std::optional<ride_log::LiveExtent> live = writer_.liveExtent()) {
    facts.recordingBoot = live->boot;
  } else if (counters_.cardMounted.load(std::memory_order_relaxed)) {
    // Mounted, and the writer hasn't opened its part yet (or it just closed one that failed).
    facts.rideLogStarting = true;
  }
  return facts;
}

std::optional<files::FreeSpace> BikeCard::freeSpace() {
  // Under the lock, so the card can't be unmounted mid-query.
  const std::lock_guard<std::mutex> lock(access_.mountLock());
  if (!access_.state().mounted) return std::nullopt;
  const std::optional<CardUsage> usage = cardUsage();
  if (!usage) return std::nullopt;
  return files::FreeSpace{usage->freeBytes, usage->totalBytes};
}

void BikeCard::changed(bool) {
  // Freed space or deleted parts: the writer counts its parts again, and retries a next part it had
  // no room for.
  writer_.cardChanged();
}

CardFiles& cardFiles() {
  static CardFiles shared;
  return shared;
}

void setCardFiles(files::Card* card) { cardFiles().card = card; }

}  // namespace emcool
