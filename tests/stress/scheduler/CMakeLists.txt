include(GoogleTest)

add_executable(scheduler_stress_tests
  concurrency_test.cpp
  churn_test.cpp
  scale_test.cpp
)

target_include_directories(scheduler_stress_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/tests/unit/scheduler
)
target_link_libraries(scheduler_stress_tests PRIVATE tcu_scheduler GTest::gtest_main)

# Duration comes from TCU_SCHEDULER_STRESS_SECONDS (default 5), so CI finishes;
# the CTest timeout must leave room for a deliberate soak run.
gtest_discover_tests(scheduler_stress_tests
  PROPERTIES LABELS "scheduler-stress" TIMEOUT 900)
