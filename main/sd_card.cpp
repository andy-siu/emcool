#include "sd_card.hpp"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "emcool/log.hpp"
#include "esp_vfs_fat.h"

// Follows ESP-IDF's examples/storage/sd_card/sdspi.

namespace emcool {

namespace {

constexpr const char* kTag = "sd";
constexpr spi_host_device_t kSpiHost = SPI3_HOST;

}  // namespace

bool SdCard::mount(int sckGpio, int mosiGpio, int misoGpio, int csGpio) {
  if (card_ != nullptr) {
    return true;
  }
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kSpiHost;

  if (!busInitialized_) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = mosiGpio;
    bus.miso_io_num = misoGpio;
    bus.sclk_io_num = sckGpio;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    const esp_err_t result = spi_bus_initialize(kSpiHost, &bus, SDSPI_DEFAULT_DMA);
    if (result != ESP_OK) {
      logError(kTag, "spi_bus_initialize(SPI3, sck=%d mosi=%d miso=%d) failed: %s", sckGpio, mosiGpio, misoGpio,
               esp_err_to_name(result));
      return false;
    }
    busInitialized_ = true;
  }

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = static_cast<gpio_num_t>(csGpio);
  slot.host_id = kSpiHost;

  esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
  mountConfig.format_if_mount_failed = false;
  // The current part, the next part being prepared, and a directory listing.
  mountConfig.max_files = 4;
  mountConfig.allocation_unit_size = 16 * 1024;

  const esp_err_t result = esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot, &mountConfig, &card_);
  if (result != ESP_OK) {
    card_ = nullptr;
    if (result == ESP_FAIL) {
      logWarning(kTag, "the card is there but has no FAT filesystem it can mount; format it FAT32 on the laptop");
    } else {
      logWarning(kTag, "no card (cs=GPIO%d): %s", csGpio, esp_err_to_name(result));
    }
    return false;
  }
  logInfo(kTag, "mounted %s: %.8s, %llu MB", kSdMountPoint, card_->cid.name,
          static_cast<unsigned long long>(card_->csd.capacity) * card_->csd.sector_size / (1024 * 1024));
  return true;
}

std::optional<CardUsage> cardUsage() {
  CardUsage usage = {};
  const esp_err_t result = esp_vfs_fat_info(kSdMountPoint, &usage.totalBytes, &usage.freeBytes);
  if (result != ESP_OK) {
    return std::nullopt;
  }
  return usage;
}

std::optional<std::uint64_t> FatCardSpace::freeBytes() {
  const std::optional<CardUsage> usage = cardUsage();
  if (!usage) {
    logWarning(kTag, "esp_vfs_fat_info could not read the card's free space");
    return std::nullopt;
  }
  return usage->freeBytes;
}

void SdCard::unmount() {
  if (card_ == nullptr) {
    return;
  }
  const esp_err_t result = esp_vfs_fat_sdcard_unmount(kSdMountPoint, card_);
  if (result != ESP_OK) {
    logWarning(kTag, "unmount failed: %s", esp_err_to_name(result));
  }
  card_ = nullptr;
}

}  // namespace emcool
