# Host build of the platform-independent components, for checks that run on the laptop.
# tools/check.sh drives it; it never needs ESP-IDF.
cmake_minimum_required(VERSION 3.22)
project(emcool_host CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)
# No fused multiply-add: an FMA changes the last bit of a*b+c, which breaks bit-exact parity with
# the reference implementation's JavaScript (V8 does not contract). The charge-auto least-squares
# slope is the first place it bit; keeping it off project-wide is the rule, not the exception.
add_compile_options(-ffp-contract=off)

# OpenSSL 3 for the host crypto backend; Homebrew keeps it out of the default search path.
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL 3 REQUIRED)
find_package(ZLIB REQUIRED)

add_subdirectory(../components/emcool_log emcool_log)
add_subdirectory(../components/emcool_text emcool_text)
add_subdirectory(../components/emcool_can emcool_can)
add_subdirectory(../components/emcool_signals emcool_signals)
add_subdirectory(../components/emcool_clock emcool_clock)

add_executable(check_decoders tests/check_decoders.cpp)
target_link_libraries(check_decoders PRIVATE emcool_can emcool_signals)

add_executable(check_signals tests/check_signals.cpp)
target_link_libraries(check_signals PRIVATE emcool_can emcool_signals)

add_executable(check_clock tests/check_clock.cpp)
target_link_libraries(check_clock PRIVATE emcool_clock)

add_subdirectory(../components/emcool_ride_log emcool_ride_log)

add_executable(check_ride_log tests/check_ride_log.cpp)
target_link_libraries(check_ride_log PRIVATE emcool_can emcool_ride_log)

add_subdirectory(../components/emcool_ble_wire emcool_ble_wire)

add_executable(check_ble_wire tests/check_ble_wire.cpp)
target_link_libraries(check_ble_wire PRIVATE emcool_ble_wire ZLIB::ZLIB)

add_subdirectory(../components/emcool_obd emcool_obd)
add_subdirectory(../components/emcool_async emcool_async)
add_subdirectory(../components/emcool_vcu emcool_vcu)
add_subdirectory(../components/emcool_settings emcool_settings)
add_subdirectory(../components/emcool_ble_server emcool_ble_server)

add_executable(ble_server_host tests/ble_server_host.cpp)
target_link_libraries(ble_server_host PRIVATE emcool_ble_server emcool_can emcool_settings)

add_executable(check_pack_temperature tests/check_pack_temperature.cpp)
target_link_libraries(check_pack_temperature PRIVATE emcool_can)

add_executable(check_settings tests/check_settings.cpp)
target_link_libraries(check_settings PRIVATE emcool_settings)

add_executable(check_console tests/check_console.cpp)
target_link_libraries(check_console PRIVATE emcool_ble_server emcool_settings)



add_executable(check_obd tests/check_obd.cpp)
target_link_libraries(check_obd PRIVATE emcool_obd)


add_executable(check_async tests/check_async.cpp)
target_link_libraries(check_async PRIVATE emcool_async)

add_executable(check_text tests/check_text.cpp)
target_link_libraries(check_text PRIVATE emcool_text)


add_executable(check_vcu tests/check_vcu.cpp)
target_link_libraries(check_vcu PRIVATE emcool_vcu)

add_executable(check_service tests/check_service.cpp)
target_link_libraries(check_service PRIVATE emcool_vcu)

add_executable(check_diagnostics tests/check_diagnostics.cpp)
target_link_libraries(check_diagnostics PRIVATE emcool_vcu)

add_executable(check_write tests/check_write.cpp)
target_link_libraries(check_write PRIVATE emcool_vcu)

add_executable(check_write_session tests/check_write_session.cpp)
target_link_libraries(check_write_session PRIVATE emcool_vcu)

add_executable(check_charge_auto tests/check_charge_auto.cpp)
target_link_libraries(check_charge_auto PRIVATE emcool_vcu)
