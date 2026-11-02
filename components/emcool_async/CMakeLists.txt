# An event loop with Node's ordering rules and C++20 coroutines on it, for porting cool-eva's
# async TypeScript (the VCU service tools) so it reads, and runs, like the original.
set(emcool_async_sources
  src/loop.cpp
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${emcool_async_sources} INCLUDE_DIRS include REQUIRES emcool_obd)
else()
  add_library(emcool_async STATIC ${emcool_async_sources})
  target_include_directories(emcool_async PUBLIC include)
  target_link_libraries(emcool_async PUBLIC emcool_obd)
endif()
