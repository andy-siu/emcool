# The bike's end of the phone link: session, live stream and requests, ported from cool-eva
# src/ble-server/. Transport-free, so host/ drives it with cool-eva's own phone-side code.
set(emcool_ble_server_sources
  src/endpoints.cpp
  src/server.cpp
  src/telemetry_stream.cpp
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_ble_server_sources} INCLUDE_DIRS include
                         REQUIRES emcool_ble_wire emcool_log emcool_obd emcool_signals emcool_text emcool_vcu)
else()
  add_library(emcool_ble_server STATIC ${emcool_ble_server_sources})
  target_include_directories(emcool_ble_server PUBLIC include)
  target_link_libraries(emcool_ble_server PUBLIC emcool_ble_wire emcool_log emcool_obd emcool_signals emcool_text emcool_vcu)
endif()
