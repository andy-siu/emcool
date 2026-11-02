# The GPS clock gate and clock-trust state, ported from cool-eva src/gps/clock*.ts. Pure:
# the firmware does the actual settimeofday.
set(emcool_clock_sources
  src/gps_clock_gate.cpp
  src/system_clock_sync.cpp
)

# Same doubles as the TypeScript, which the differential check compares exactly.
set(emcool_clock_options -ffp-contract=off)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_clock_sources} INCLUDE_DIRS include REQUIRES emcool_log)
  target_compile_options(${COMPONENT_LIB} PRIVATE ${emcool_clock_options})
else()
  add_library(emcool_clock STATIC ${emcool_clock_sources})
  target_include_directories(emcool_clock PUBLIC include)
  target_link_libraries(emcool_clock PUBLIC emcool_log)
  target_compile_options(emcool_clock PRIVATE ${emcool_clock_options})
endif()
