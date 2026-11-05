include(GoogleTest)

add_executable(config_logging_stress_tests
  log_flood_test.cpp
  reload_storm_test.cpp
  soak_test.cpp
)

target_include_directories(config_logging_stress_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/tests/unit/logging
  ${CMAKE_SOURCE_DIR}/tests/unit/config
  # project 02 structural JSON checker, reused rather than written again
  # (Constitution Principle II). It is a standalone header with no link
  # dependency, so this costs nothing but an include path.
  ${CMAKE_SOURCE_DIR}/apps/vehicle_service
)
target_compile_definitions(config_logging_stress_tests PRIVATE
  TCU_CONFIGS_DIR="${CMAKE_SOURCE_DIR}/configs")
target_link_libraries(config_logging_stress_tests
  PRIVATE tcu_config tcu_logging tcu_scheduler GTest::gtest_main)

# Duration comes from TCU_CONFIG_STRESS_SECONDS (default 5) so CI finishes; the
# CTest timeout must leave room for a deliberate soak run (research R-21).
gtest_discover_tests(config_logging_stress_tests
  PROPERTIES LABELS "config-logging-stress" TIMEOUT 900)
