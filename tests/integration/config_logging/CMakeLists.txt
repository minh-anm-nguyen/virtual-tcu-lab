include(GoogleTest)

add_executable(config_logging_integration_tests
  startup_test.cpp
  hot_reload_test.cpp
  rejection_test.cpp
  shutdown_test.cpp
  serialized_reload_test.cpp
  event_bus_test.cpp
)

target_include_directories(config_logging_integration_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/tests/unit/logging
  ${CMAKE_SOURCE_DIR}/tests/unit/config
)
target_compile_definitions(config_logging_integration_tests PRIVATE
  TCU_CONFIGS_DIR="${CMAKE_SOURCE_DIR}/configs")
target_link_libraries(config_logging_integration_tests
  PRIVATE tcu_config tcu_logging tcu_scheduler tcu_eventbus GTest::gtest_main)

gtest_discover_tests(config_logging_integration_tests
  PROPERTIES LABELS "config-logging-integration" TIMEOUT 120)
