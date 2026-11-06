add_executable(dbc_decoder_stress_tests
  soak_test.cpp
)

target_link_libraries(dbc_decoder_stress_tests PRIVATE
  tcu_signal tcu_dbc tcu_can tcu_scheduler tcu_logging gtest_main
)

target_include_directories(dbc_decoder_stress_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/signal/src
  ${CMAKE_SOURCE_DIR}/tests/unit/signal
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tests/support
)

target_compile_definitions(dbc_decoder_stress_tests PRIVATE
  TCU_DBC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/dbc"
  TCU_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs"
)

gtest_discover_tests(dbc_decoder_stress_tests PROPERTIES LABELS "dbc-stress")
