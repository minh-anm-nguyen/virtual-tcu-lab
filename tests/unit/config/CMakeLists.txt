include(GoogleTest)

add_executable(config_unit_tests
  schema_test.cpp
  loader_test.cpp
  validator_test.cpp
  snapshot_test.cpp
  reload_test.cpp
  subscriber_test.cpp
  poller_test.cpp
  lock_test.cpp
  metrics_test.cpp
)

# Private headers under test (json_document, loader, validator, snapshot_diff,
# file_poller); TCU_CONFIGS_DIR lets a test read the shipped fixtures without
# assuming the working directory.
target_include_directories(config_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/config/src
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(config_unit_tests PRIVATE
  TCU_CONFIGS_DIR="${CMAKE_SOURCE_DIR}/configs")
target_link_libraries(config_unit_tests PRIVATE tcu_config GTest::gtest_main)

gtest_discover_tests(config_unit_tests PROPERTIES LABELS "config-unit" TIMEOUT 60)
