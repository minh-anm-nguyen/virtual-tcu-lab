include(GoogleTest)

add_executable(logging_unit_tests
  formatter_test.cpp
  redaction_test.cpp
  queue_test.cpp
  filter_test.cpp
  console_sink_test.cpp
  file_sink_test.cpp
  lifecycle_test.cpp
  metrics_test.cpp
)

# Private headers of the module are under test (json_formatter, log_queue,
# redaction); tests/ carries the shared TestGate of project 01.
target_include_directories(logging_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/logging/src
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(logging_unit_tests PRIVATE tcu_logging GTest::gtest_main)

# TIMEOUT is mandatory, not cosmetic: a logger bug shows up as a hang (a
# producer waiting on a full queue, a self-join in stop()), and a hang must
# fail loudly.
gtest_discover_tests(logging_unit_tests PROPERTIES LABELS "logging-unit" TIMEOUT 60)
