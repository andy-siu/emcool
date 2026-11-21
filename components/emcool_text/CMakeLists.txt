# Allocation-free text building (JSON pieces, numbers in their shortest round-trip form),
# shared by the ride log, the Bluetooth server and the diagnostics; BigText, for text too big for
# internal RAM, in PSRAM on the bike; JavaScript's number formats; and the consoles' `help` layout.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/big_text.cpp src/big_text_esp.cpp src/help.cpp src/js.cpp src/text_buffer.cpp
                         INCLUDE_DIRS include REQUIRES heap)
else()
  add_library(emcool_text STATIC src/big_text.cpp src/big_text_host.cpp src/help.cpp src/js.cpp src/text_buffer.cpp)
  target_include_directories(emcool_text PUBLIC include)
endif()
