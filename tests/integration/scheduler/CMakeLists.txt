include(GoogleTest)

add_executable(scheduler_integration_tests
  backpressure_test.cpp
  shutdown_test.cpp
  event_bus_test.cpp
)

# tests/ carries the shared TestGate; tests/unit/scheduler carries the fixture,
# the Recorder and the CountingClock, all reused here rather than duplicated.
target_include_directories(scheduler_integration_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/tests/unit/scheduler
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(scheduler_integration_tests
  PRIVATE tcu_scheduler tcu_eventbus GTest::gtest_main)

# IT-04 drives a real project 02 IPC server, which is Linux-only (research
# R-00 of specs/002-*). Everything else in this suite is portable.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(scheduler_integration_tests PRIVATE ipc_deadline_test.cpp)
  target_link_libraries(scheduler_integration_tests PRIVATE tcu_ipc)
  # Project 02 already owns a per-test socket directory helper; reuse it
  # rather than writing a second one (Principle II).
  target_include_directories(scheduler_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/unit/ipc)
else()
  message(STATUS "Scheduler IT-04 (IPC deadline) skipped: Linux only")
endif()

gtest_discover_tests(scheduler_integration_tests
  PROPERTIES LABELS "scheduler-integration" TIMEOUT 120)
