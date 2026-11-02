# The per-bike settings and the console commands that change them. Platform-free; the
# firmware supplies NVS storage, the host checks an in-memory one.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/settings.cpp INCLUDE_DIRS include REQUIRES emcool_log)
else()
  add_library(emcool_settings STATIC src/settings.cpp)
  target_include_directories(emcool_settings PUBLIC include)
  target_link_libraries(emcool_settings PUBLIC emcool_log)
endif()
