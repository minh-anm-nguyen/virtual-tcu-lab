# Project 06: libtcu_signal. Everything with a lifecycle.
#
# Links tcu_can (project 05) because ICanTransport and CanMessage appear in its
# public headers: project 06 reuses that transport rather than defining a frame
# source of its own (research R-22). That makes this target Linux-only, like
# projects 02 and 05 - tcu_dbc stays portable (research R-00).
add_library(tcu_signal STATIC
  src/fault_hooks.cpp
  src/signal_cache.cpp
  src/decoder_config.cpp
  src/decoder_metrics.cpp
)

target_include_directories(tcu_signal PUBLIC include PRIVATE src)
target_compile_features(tcu_signal PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_signal PRIVATE -Wall -Wextra -Wconversion)
endif()

find_package(Threads REQUIRED)

target_link_libraries(tcu_signal
  PUBLIC tcu_dbc tcu_can tcu_scheduler tcu_logging Threads::Threads
  PRIVATE nlohmann_json::nlohmann_json
)
