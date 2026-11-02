# The phone link's wire protocol, ported from cool-eva public/lib/ble-wire.js and
# src/ble-server/session.ts. Pure apart from crypto and inflate, which have one backend per
# platform: mbedTLS PSA and the ROM's tinfl on the ESP32, OpenSSL and zlib on the host.
set(emcool_ble_wire_sources
  src/crypto_self_test.cpp
  src/deflate.cpp
  src/json_reader.cpp
  src/session.cpp
  src/wire.cpp
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_ble_wire_sources} src/crypto_psa.cpp src/inflate_rom.cpp
                         INCLUDE_DIRS include
                         REQUIRES emcool_log emcool_text esp_hw_support esp_rom mbedtls)
else()
  find_package(OpenSSL 3 REQUIRED)
  find_package(ZLIB REQUIRED)
  add_library(emcool_ble_wire STATIC ${emcool_ble_wire_sources} src/crypto_openssl.cpp src/inflate_zlib.cpp)
  target_include_directories(emcool_ble_wire PUBLIC include)
  target_link_libraries(emcool_ble_wire PUBLIC emcool_log emcool_text PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
endif()
