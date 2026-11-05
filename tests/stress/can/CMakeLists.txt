include(GoogleTest)

# Project 05 stress and fuzz suite, spec section 18.3 and B-30.
#
# Runs entirely on FakeCanTransport: no vcan needed, and the malformed frames it
# injects could not be carried by a real bus anyway (spec section 18.1 note).
add_executable(can_stress_tests
  can_stress_test.cpp
)

target_include_directories(can_stress_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/can/src
  ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(can_stress_tests PRIVATE tcu_can GTest::gtest_main)

# Generous, because 50,000 frames through two threads under a sanitizer is slow
# - but bounded, because a deadlock has to fail rather than hang (B-30).
gtest_discover_tests(can_stress_tests PROPERTIES LABELS "can-stress" TIMEOUT 300)
