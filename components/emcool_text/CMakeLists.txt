# Allocation-free text building (JSON pieces, numbers in their shortest round-trip form),
# shared by the ride log, the Bluetooth server and the diagnostics; JavaScript's number formats; and
# the consoles' `help` layout.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/help.cpp src/js.cpp src/text_buffer.cpp INCLUDE_DIRS include)
else()
  add_library(emcool_text STATIC src/help.cpp src/js.cpp src/text_buffer.cpp)
  target_include_directories(emcool_text PUBLIC include)
endif()
