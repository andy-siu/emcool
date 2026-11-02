# Allocation-free text building (JSON pieces, numbers in their shortest round-trip form),
# shared by the ride log, the Bluetooth server and the diagnostics; and JavaScript's number formats.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/js.cpp src/text_buffer.cpp INCLUDE_DIRS include)
else()
  add_library(emcool_text STATIC src/js.cpp src/text_buffer.cpp)
  target_include_directories(emcool_text PUBLIC include)
endif()
