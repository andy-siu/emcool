# The console's commands for the microSD card: dir, cd, type, mkdir, rmdir, rm, cp and mv. Plain
# POSIX calls, which ESP-IDF maps onto FatFs, so host/ runs them on a folder of the laptop's.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/card_shell.cpp INCLUDE_DIRS include REQUIRES emcool_log vfs)
else()
  add_library(emcool_files STATIC src/card_shell.cpp)
  target_include_directories(emcool_files PUBLIC include)
  target_link_libraries(emcool_files PUBLIC emcool_log)
endif()
