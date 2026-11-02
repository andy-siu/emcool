# The signal registry (generated from cool-eva by tools/generate-registry.mts), live state,
# and the deadband gate in front of the ride log.
set(emcool_signals_sources
  src/registry.gen.cpp
  src/signal_store.cpp
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_signals_sources} INCLUDE_DIRS include REQUIRES emcool_log)
else()
  add_library(emcool_signals STATIC ${emcool_signals_sources})
  target_include_directories(emcool_signals PUBLIC include)
  target_link_libraries(emcool_signals PUBLIC emcool_log)
endif()
