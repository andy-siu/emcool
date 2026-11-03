# The per-bike settings, the console commands that change them, and the Wi-Fi console's login and
# line handling. Platform-free; the firmware supplies NVS storage and sockets, the host checks fakes.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/remote_console.cpp src/settings.cpp INCLUDE_DIRS include REQUIRES emcool_log)
else()
  add_library(emcool_settings STATIC src/remote_console.cpp src/settings.cpp)
  target_include_directories(emcool_settings PUBLIC include)
  target_link_libraries(emcool_settings PUBLIC emcool_log)
endif()
