add_executable(dbc_unit_tests
  sha256_test.cpp
  parser_test.cpp
  validator_test.cpp
  database_test.cpp
)

target_link_libraries(dbc_unit_tests PRIVATE tcu_dbc gtest_main)

# src/ is on the include path so private headers (sha256, lexer, validator) are
# directly testable without widening the public surface.
target_include_directories(dbc_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/dbc/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tests/support
)

target_compile_definitions(dbc_unit_tests PRIVATE
  TCU_DBC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/dbc"
)

gtest_discover_tests(dbc_unit_tests PROPERTIES LABELS "dbc-unit")
