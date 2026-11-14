#include "sd_card.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "emcool/log.hpp"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Follows ESP-IDF's examples/storage/sd_card/sdspi.

namespace emcool {

namespace {

constexpr const char* kTag = "sd";
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr int kMountAttempts = 5;

// One full-duplex exchange of up to four bytes; what came back on MISO in `in`.
void exchange(spi_device_handle_t device, const std::uint8_t* out, std::uint8_t* in, std::size_t length) {
  spi_transaction_t transfer = {};
  transfer.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
  transfer.length = length * 8;
  std::memcpy(transfer.tx_data, out, length);
  if (spi_device_polling_transmit(device, &transfer) == ESP_OK && in != nullptr) {
    std::memcpy(in, transfer.rx_data, length);
  }
}

// The card keeps its power when the ESP32 resets (flashing, the USB serial's reset line, a crash),
// so it can be left part-way through whatever it was doing, and then it doesn't answer the next
// CMD0: the mount times out, on every boot until the card loses power. Measured on the bench:
// 0 of 6 resets mounted without this, every one of 8 with it and the retry in mount(). Finish
// what a card mid-write would be waiting for, with CS low: the rest of a 512-byte block and its
// CRC as 0xFF, the multi-block write's stop token, then wait out its busy. A card that was idle
// ignores all of that as filler (0xFD isn't a command: those start 01). Then CS high and the
// usual run of clocks.
void releaseStuckCard(int csGpio) {
  spi_device_interface_config_t config = {};
  config.clock_speed_hz = 400 * 1000;
  config.mode = 0;
  config.spics_io_num = -1;  // CS by hand, so it can stay low across the whole sequence
  config.queue_size = 1;
  spi_device_handle_t device = nullptr;
  if (spi_bus_add_device(kSpiHost, &config, &device) != ESP_OK) {
    logWarning(kTag, "couldn't take the SPI bus to free a stuck card; mounting anyway");
    return;
  }
  const gpio_num_t cs = static_cast<gpio_num_t>(csGpio);
  gpio_set_level(cs, 1);
  gpio_set_direction(cs, GPIO_MODE_OUTPUT);

  const std::uint8_t idle[4] = {0xff, 0xff, 0xff, 0xff};
  std::uint8_t in[4] = {};
  unsigned answered = 0;  // bytes the card drove to something other than 0xFF
  gpio_set_level(cs, 0);
  // A whole block's worth and its CRC, and a margin.
  for (int i = 0; i < 132; i++) {
    exchange(device, idle, in, 4);
    for (const std::uint8_t byte : in) answered += byte != 0xff;
  }
  const std::uint8_t stopToken[4] = {0xfd, 0xff, 0xff, 0xff};
  exchange(device, stopToken, nullptr, 4);
  // Busy is MISO held low; a card finishing a write can take a few hundred ms.
  const std::int64_t deadline = esp_timer_get_time() + 500 * 1000;
  bool busy = true;
  while (esp_timer_get_time() < deadline) {
    exchange(device, idle, in, 1);
    if (in[0] == 0xff) {
      busy = false;
      break;
    }
    vTaskDelay(1);
  }
  gpio_set_level(cs, 1);
  for (int i = 0; i < 4; i++) exchange(device, idle, nullptr, 4);
  spi_bus_remove_device(device);
  if (busy) logWarning(kTag, "the card was still busy 500 ms after being released (%u bytes answered)", answered);
}

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
  // The ride log's current part and the next being prepared, service mode's snapshot files (a copy
  // takes two), the Bluetooth and Wi-Fi ride-log downloads' one each, and a console's cp (two).
  // Directory listings don't count against it. Each slot costs a few dozen bytes until it's used;
  // an open file takes its 4 KB sector buffer.
  mountConfig.max_files = 8;
  mountConfig.allocation_unit_size = 16 * 1024;

  esp_err_t result = ESP_FAIL;
  for (int attempt = 1; attempt <= kMountAttempts; attempt++) {
    releaseStuckCard(csGpio);
    result = esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot, &mountConfig, &card_);
    // Only a card that didn't answer is worth asking again; one without a filesystem won't grow one.
    if (result != ESP_ERR_TIMEOUT) break;
    if (attempt < kMountAttempts) logInfo(kTag, "the card didn't answer; trying again (%d of %d)", attempt + 1, kMountAttempts);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
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
