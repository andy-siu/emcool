# Over-the-air updates: an image checked and streamed into the spare app slot. Platform-free, so
# host/ runs it on a real build's emcool.bin; the firmware supplies the slot (esp_ota_*).
if(ESP_PLATFORM)
  idf_component_register(SRCS src/firmware_update.cpp INCLUDE_DIRS include REQUIRES emcool_log)
else()
  add_library(emcool_update STATIC src/firmware_update.cpp)
  target_include_directories(emcool_update PUBLIC include)
  target_link_libraries(emcool_update PUBLIC emcool_log)
endif()
