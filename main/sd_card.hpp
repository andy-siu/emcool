#pragma once

// The microSD card on SPI3, mounted as FAT at /sdcard. Never formatted automatically: a card
// that won't mount stays untouched for the laptop to look at.

#include <cstdint>
#include <optional>

#include "emcool/ride_log/log_writer.hpp"
#include "sdmmc_cmd.h"

namespace emcool {

inline constexpr const char* kSdMountPoint = "/sdcard";

class SdCard {
 public:
  bool mount(int sckGpio, int mosiGpio, int misoGpio, int csGpio);
  void unmount();
  bool mounted() const { return card_ != nullptr; }

 private:
  sdmmc_card_t* card_ = nullptr;
  bool busInitialized_ = false;
};

struct CardUsage {
  std::uint64_t freeBytes;
  std::uint64_t totalBytes;
};

// nullopt when no card is mounted.
std::optional<CardUsage> cardUsage();

// Free space as FatFs reports it. FatFs trusts the card's FSInfo count (FF_FS_NOFSINFO 0),
// so this doesn't scan the whole FAT.
class FatCardSpace final : public ride_log::CardSpace {
 public:
  std::optional<std::uint64_t> freeBytes() override;
};

}  // namespace emcool
