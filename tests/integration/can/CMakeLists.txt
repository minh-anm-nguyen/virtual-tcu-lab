include(GoogleTest)

# Project 05 integration suite: the real SocketCAN path on a real vcan0.
#
# Every case SKIPS when vcan0 is unavailable rather than failing
# (specs/005-virtual-can-bus-laboratory/research.md R-01). The stock WSL2 kernel
# ships no vcan module, so a failing suite would be red on every developer
# machine here; .github/workflows/ci.yml provisions the interface, which is
# where these actually run.
add_executable(can_integration_tests
  vcan_roundtrip_test.cpp
  vcan_lifecycle_test.cpp
)

target_include_directories(can_integration_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(can_integration_tests PRIVATE tcu_can GTest::gtest_main)

# A longer timeout than the unit suite: these wait on real kernel delivery and
# on interface state changes. Still a timeout, because a lost wakeup must fail
# rather than hang (NFR-11).
gtest_discover_tests(can_integration_tests PROPERTIES LABELS "can-integration" TIMEOUT 120)
