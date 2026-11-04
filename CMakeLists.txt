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

if(TCU_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks/event_bus_bench)
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
endif()
