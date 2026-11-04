add_executable(ipc_integration_tests
  request_response_test.cpp
  notification_test.cpp
  reconnect_test.cpp
  multi_client_test.cpp
  server_lifecycle_test.cpp
)

target_include_directories(ipc_integration_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/unit/ipc
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ipc_integration_tests PRIVATE tcu_ipc gtest_main)

# The tests fork the real server binary, so it must be built first.
add_dependencies(ipc_integration_tests vehicle_service)
target_compile_definitions(ipc_integration_tests PRIVATE
  TCU_VEHICLE_SERVICE_BIN="$<TARGET_FILE:vehicle_service>")

include(GoogleTest)
gtest_discover_tests(ipc_integration_tests PROPERTIES LABELS "ipc-integration")
