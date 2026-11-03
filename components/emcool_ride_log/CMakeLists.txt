# The ride log: segments built by the decode task, records written to the card by the writer
# task, and read back off it for the dashboard's /dl. Platform-neutral — POSIX file calls that ESP-IDF maps onto FatFs — so host/ runs it.
set(emcool_ride_log_sources
  src/card_layout.cpp
  src/crc32.cpp
  src/download.cpp
  src/log_writer.cpp
  src/record.cpp
  src/ride_log.cpp
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_ride_log_sources} INCLUDE_DIRS include
                         REQUIRES emcool_clock emcool_log emcool_signals emcool_text vfs)
else()
  add_library(emcool_ride_log STATIC ${emcool_ride_log_sources})
  target_include_directories(emcool_ride_log PUBLIC include)
  target_link_libraries(emcool_ride_log PUBLIC emcool_clock emcool_log emcool_signals emcool_text)
endif()
