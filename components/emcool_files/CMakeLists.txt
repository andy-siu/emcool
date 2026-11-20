# The console's commands for the microSD card (dir, cd, type, mkdir, rmdir, rm, cp and mv), and the
# SSH console's SFTP server for it. Plain POSIX calls, which ESP-IDF maps onto FatFs, so host/ runs
# them on a folder of the laptop's.
if(ESP_PLATFORM)
  idf_component_register(SRCS src/card_shell.cpp src/sftp_server.cpp src/sftp_memory_esp.cpp INCLUDE_DIRS include
                         REQUIRES emcool_log emcool_text vfs heap)
else()
  add_library(emcool_files STATIC src/card_shell.cpp src/sftp_server.cpp src/sftp_memory_host.cpp)
  target_include_directories(emcool_files PUBLIC include)
  target_link_libraries(emcool_files PUBLIC emcool_log emcool_text)
endif()
