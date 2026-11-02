# Builds under ESP-IDF for the firmware and as a plain CMake library for the host checks,
# so the same decoders log through ESP_LOG on the bike and stderr on the laptop.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/log_esp.cpp INCLUDE_DIRS include REQUIRES log)
else()
  add_library(emcool_log STATIC src/log_host.cpp)
  target_include_directories(emcool_log PUBLIC include)
endif()
