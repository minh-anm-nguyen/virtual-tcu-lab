cmake_minimum_required(VERSION 3.20)
project(virtual_tcu_lab CXX)

# Constitution: C++17, no compiler extensions in library code.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TCU_BUILD_TESTS "Build tests" ON)
option(TCU_BUILD_BENCHMARKS "Build benchmarks" ON)

option(TCU_SANITIZER_SETARCH
       "Run test binaries under 'setarch -R'. ThreadSanitizer's fixed shadow mapping clashes with the high-entropy ASLR of recent kernels (research R-00 addendum); without this a TSan build aborts at startup."
       OFF)
if(TCU_SANITIZER_SETARCH)
  # Picked up by CTest and by gtest_discover_tests, which runs the binary at
  # build time.
  set(CMAKE_CROSSCOMPILING_EMULATOR setarch -R)
endif()

enable_testing()

# Project 01. Projects 02-30 each append one add_subdirectory line here.
add_subdirectory(modules/core/event_bus)
add_subdirectory(apps/event_bus_demo)

# Project 02: IPC Service Framework. POSIX-only (AF_UNIX, poll, SO_PEERCRED,
# MSG_NOSIGNAL), so non-Linux hosts skip it - research R-00 in specs/002-*.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(modules/ipc)
  add_subdirectory(apps/vehicle_service)
  add_subdirectory(apps/tcu_client)
else()
  message(STATUS "Project 02 (IPC) skipped: Linux only")
endif()

# Project 03: Timer & Task Scheduler. Standard library only - no POSIX call
# anywhere - so it is deliberately NOT inside the Linux guard above
# (research R-00 in specs/003-timer-task-scheduler).
add_subdirectory(modules/scheduler)
add_subdirectory(apps/telemetry_timer_demo)
add_subdirectory(apps/modem_timeout_demo)

# Project 04: Configuration & Logging Service. Portable for the same reason as
# project 03 - no POSIX call anywhere, file permissions and file stat go through
# <filesystem> - so neither target sits inside the Linux guard above
# (specs/004-configuration-logging-service/research.md R-00).
#
# nlohmann/json is pinned to the v3.11.3 *single header*, fetched by URL with a
# SHA256, deliberately NOT through the upstream CMake project: that project
# declares cmake_minimum_required(VERSION 3.1), which CMake 4 refuses outright.
# Downloading one header also keeps the dependency out of every public header
# (research R-01). The download is skipped when the file is already present and
# its hash matches, so a configured tree rebuilds offline.
set(TCU_NLOHMANN_DIR ${CMAKE_BINARY_DIR}/_deps/nlohmann-single)
file(DOWNLOAD
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  ${TCU_NLOHMANN_DIR}/nlohmann/json.hpp
  EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
  TLS_VERIFY ON
)
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${TCU_NLOHMANN_DIR})

add_subdirectory(modules/logging)
add_subdirectory(modules/config)
add_subdirectory(apps/config_logging_demo)

# Project 05: Virtual CAN Bus Laboratory. Linux only - the transport is
# PF_CAN/SOCK_RAW/CAN_RAW with <linux/can.h>, plus eventfd(2) and poll(2),
# none of which has a Windows equivalent
# (specs/005-virtual-can-bus-laboratory/research.md R-00). Same reason project
# 02 sits in a guard of its own.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(modules/can)
  add_subdirectory(apps/tcu_can_service)
  add_subdirectory(apps/vehicle_simulator)
  add_subdirectory(apps/bcm_simulator)
  add_subdirectory(apps/can_control_cli)
  # Project 06's lifecycle half links tcu_can, so it lives here too
  # (specs/006-dbc-signal-decoder/research.md R-00, R-22).
  add_subdirectory(modules/signal)
  add_subdirectory(apps/virtual_dbc_demo)
else()
  message(STATUS "Project 05 (CAN) skipped: Linux only")
endif()

# Project 06: DBC Signal Decoder. tcu_dbc is portable - no POSIX call, so it
# sits outside every platform guard, exactly like projects 03 and 04
# (specs/006-dbc-signal-decoder/research.md R-00). tcu_signal links tcu_can and
# is therefore Linux-only, in the guard below.
add_subdirectory(modules/dbc)
add_subdirectory(apps/dbc_validate)
add_subdirectory(apps/dbc_decode)

if(TCU_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks/event_bus_bench)
  add_subdirectory(benchmarks/scheduler_bench)
  # Project 06's benchmark links only tcu_dbc, so it stays portable.
  add_subdirectory(benchmarks/dbc_decoder_bench)
  # Project 05's benchmark needs SocketCAN headers through tcu_can, so it sits
  # inside the same platform guard as the library.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(benchmarks/can_bench)
  endif()
endif()

if(TCU_BUILD_TESTS)
  include(FetchContent)
  # Pinned per research R-14: reproducible builds, which NFR-06 depends on.
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests/unit/event_bus)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests/unit/ipc)
    add_subdirectory(tests/integration/ipc)
    add_subdirectory(tests/fault_injection/ipc)
  endif()
  add_subdirectory(tests/unit/scheduler)
  add_subdirectory(tests/integration/scheduler)
  add_subdirectory(tests/stress/scheduler)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests/unit/can)
    add_subdirectory(tests/integration/can)
    add_subdirectory(tests/stress/can)
  endif()
  add_subdirectory(tests/unit/logging)
  add_subdirectory(tests/unit/config)
  add_subdirectory(tests/integration/config_logging)
  add_subdirectory(tests/stress/config_logging)
  add_subdirectory(tests/unit/dbc)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tests/unit/signal)
    add_subdirectory(tests/integration/dbc_decoder)
    add_subdirectory(tests/stress/dbc_decoder)
  endif()
endif()
