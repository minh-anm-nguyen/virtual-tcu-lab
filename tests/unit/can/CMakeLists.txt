include(GoogleTest)

# One target for the whole project 05 unit suite, labelled so it can be run
# alone. Everything here runs without vcan: the transport under test is
# FakeCanTransport and time comes from FakeClockAdapter, so the unit and
# concurrency evidence does not depend on a kernel module
# (specs/005-virtual-can-bus-laboratory/research.md R-01, R-25).
add_executable(can_unit_tests
  frame_catalog_test.cpp
  frame_validator_test.cpp
  bounded_frame_queue_test.cpp
  fake_can_transport_test.cpp
  config_loader_test.cpp
  demo_signal_decoder_test.cpp
  signal_cache_test.cpp
  command_correlator_test.cpp
  can_service_test.cpp
  event_adapter_test.cpp
)

# Private headers of the module are under test (bounded_frame_queue,
# can_config_schema, command_correlator); tests/ carries the shared TestGate.
target_include_directories(can_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/can/src
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(can_unit_tests PRIVATE tcu_can tcu_eventbus GTest::gtest_main GTest::gmock)

# ConfigLoader.SampleFileMatchesTable reads the real config/can_lab.json rather
# than a copy, so the shipped file and the schema table cannot drift apart.
target_compile_definitions(can_unit_tests PRIVATE
  TCU_CAN_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
)

# The TIMEOUT is mandatory: a bug in this project shows up as a hang - a poll()
# that never wakes, a join that never returns, a lock held across a callback -
# and a hang must fail loudly rather than block CI (NFR-11).
gtest_discover_tests(can_unit_tests PROPERTIES LABELS "can-unit" TIMEOUT 60)

# NFR-10, research R-02. Two mechanical checks, because "no Linux header in a
# public header" is a property of the include graph rather than something a
# review reliably catches.
#
# 1. No public header names a Linux header at all.
add_test(NAME can_public_headers_are_os_neutral
  COMMAND ${CMAKE_COMMAND}
          -D "CAN_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/modules/can/include"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/check_header_hygiene.cmake"
)
set_tests_properties(can_public_headers_are_os_neutral PROPERTIES LABELS "can-unit" TIMEOUT 30)

# 2. Every public header compiles on its own. One translation unit per header,
# generated here, so a header that silently depends on another one being
# included first fails the build rather than working by luck.
file(GLOB CAN_PUBLIC_HEADERS ${CMAKE_SOURCE_DIR}/modules/can/include/tcu/can/*.hpp)
set(CAN_STANDALONE_SOURCES "")
foreach(HEADER IN LISTS CAN_PUBLIC_HEADERS)
  get_filename_component(HEADER_NAME ${HEADER} NAME_WE)
  set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/standalone_${HEADER_NAME}.cpp)
  file(GENERATE OUTPUT ${GENERATED}
       CONTENT "#include \"tcu/can/${HEADER_NAME}.hpp\"
")
  list(APPEND CAN_STANDALONE_SOURCES ${GENERATED})
endforeach()

add_library(can_standalone_headers OBJECT ${CAN_STANDALONE_SOURCES})
target_link_libraries(can_standalone_headers PRIVATE tcu_can)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(can_standalone_headers PRIVATE -Wall -Wextra)
endif()
