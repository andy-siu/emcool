idf_component_register(
  SRCS main.cpp ble_link.cpp can_bus.cpp diagnostics_task.cpp ride_log_task.cpp sd_card.cpp settings_console.cpp status_report.cpp system_time.cpp
  INCLUDE_DIRS .
  REQUIRES
    bootloader_support
    bt
    console
    emcool_async
    emcool_ble_server
    emcool_ble_wire
    emcool_can
    emcool_clock
    emcool_log
    emcool_ride_log
    emcool_settings
    emcool_signals
    emcool_vcu
    esp_app_format
    esp_driver_sdspi
    esp_driver_spi
    esp_driver_twai
    esp_hw_support
    esp_timer
    fatfs
    freertos
    nvs_flash
    sdmmc
)
