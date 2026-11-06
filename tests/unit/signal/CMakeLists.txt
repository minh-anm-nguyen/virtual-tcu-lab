add_executable(signal_unit_tests
  frame_adapter_test.cpp
  cache_quality_test.cpp
  freshness_test.cpp
  config_test.cpp
  queue_test.cpp
)

target_link_libraries(signal_unit_tests PRIVATE tcu_signal tcu_dbc tcu_can gtest_main)

target_include_directories(signal_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/signal/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tests/support
)

target_compile_definitions(signal_unit_tests PRIVATE
  TCU_DBC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/dbc"
)

gtest_discover_tests(signal_unit_tests PROPERTIES LABELS "signal-unit")
