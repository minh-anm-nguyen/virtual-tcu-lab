# Project 05: Virtual CAN Bus Laboratory.
#
# Linux-only: the transport is PF_CAN/SOCK_RAW/CAN_RAW with <linux/can.h>,
# plus eventfd(2) and poll(2). The root CMakeLists.txt keeps this target
# inside its Linux guard, next to project 02
# (specs/005-virtual-can-bus-laboratory/research.md R-00).
#
# nlohmann/json is PRIVATE: src/json_document.hpp wraps it away, so no
# consumer of tcu_can inherits it (R-06, the same rule project 04 applied).
add_library(tcu_can STATIC
  src/json_document.cpp
  src/config_loader.cpp
  src/frame_validator.cpp
  src/demo_signal_decoder.cpp
  src/signal_cache.cpp
  src/event_bus_adapter.cpp
  src/command_correlator.cpp
  src/can_service.cpp
  src/fake_can_transport.cpp
  src/socketcan_transport.cpp
)

target_include_directories(tcu_can PUBLIC include)
target_compile_features(tcu_can PUBLIC cxx_std_17)

# src/ holds private headers; they are reachable from the sources only.
target_include_directories(tcu_can PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_can PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# tcu_scheduler is PUBLIC because ClockAdapter and Clock::time_point appear in
# can_types.hpp and can_service.hpp (R-03, R-04). tcu_logging and
# tcu_event_bus are PUBLIC because the service's dependency struct names them.
target_link_libraries(tcu_can
  PUBLIC tcu_scheduler tcu_logging tcu_eventbus Threads::Threads
  PRIVATE nlohmann_json::nlohmann_json
)
