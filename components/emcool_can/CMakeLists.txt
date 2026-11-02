# Pure frame decoders, ported from cool-eva src/can/. Builds under ESP-IDF for the firmware
# and as a plain CMake library for host/, where tests diff it against the TypeScript.
set(emcool_can_sources
  src/abs.cpp
  src/attitude.cpp
  src/charge_manager.cpp
  src/charge_setpoint.cpp
  src/decode_bms.cpp
  src/frame.cpp
  src/frame_decoder.cpp
  src/gps.cpp
  src/hub_output.cpp
  src/pack_temperature.cpp
  src/vehicle_frames.cpp
)

# No fused multiply-add: every value must match cool-eva's doubles bit for bit, and an FMA
# rounds once where JavaScript rounds twice.
set(emcool_can_options -ffp-contract=off)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_can_sources} INCLUDE_DIRS include PRIV_INCLUDE_DIRS src REQUIRES emcool_log)
  target_compile_options(${COMPONENT_LIB} PRIVATE ${emcool_can_options})
else()
  add_library(emcool_can STATIC ${emcool_can_sources})
  target_include_directories(emcool_can PUBLIC include PRIVATE src)
  target_link_libraries(emcool_can PUBLIC emcool_log)
  target_compile_options(emcool_can PRIVATE ${emcool_can_options})
endif()
