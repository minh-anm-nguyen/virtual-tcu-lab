include(GoogleTest)

# One target for the whole unit suite, labelled so it can be run alone.
add_executable(scheduler_unit_tests
  clock_adapter_test.cpp
  lifecycle_test.cpp
  validation_test.cpp
  one_shot_test.cpp
  deadline_queue_test.cpp
  ready_queue_test.cpp
  capacity_test.cpp
  lock_test.cpp
  periodic_test.cpp
  callback_error_test.cpp
  cancel_test.cpp
  metrics_test.cpp
  log_test.cpp
)

# Private headers of the module are under test (deadline_queue, ready_queue,
# log, metrics); tests/ carries the shared TestGate of project 01.
target_include_directories(scheduler_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/scheduler/src
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(scheduler_unit_tests PRIVATE tcu_scheduler GTest::gtest_main)

# The TIMEOUT is mandatory, not cosmetic: a scheduler bug shows up as a hang
# (a lost wakeup, a lock held across a callback), and a hang must fail loudly.
gtest_discover_tests(scheduler_unit_tests PROPERTIES LABELS "scheduler-unit" TIMEOUT 60)
