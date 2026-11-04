add_executable(ipc_unit_tests
  frame_codec_test.cpp
  log_test.cpp
  executor_test.cpp
  unix_socket_test.cpp
  client_request_test.cpp
  client_timeout_test.cpp
  client_lifecycle_test.cpp
  server_router_test.cpp
  server_security_test.cpp
  session_test.cpp
  metrics_test.cpp
)

# Private headers of the module are under test (executor, log, unix_socket).
target_include_directories(ipc_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/ipc/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ipc_unit_tests PRIVATE tcu_ipc gtest_main gmock)

include(GoogleTest)
gtest_discover_tests(ipc_unit_tests PROPERTIES LABELS "ipc-unit")
