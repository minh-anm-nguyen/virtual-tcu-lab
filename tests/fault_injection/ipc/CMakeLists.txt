add_executable(ipc_fault_tests
  partial_frame_test.cpp
  rapid_restart_test.cpp
  stress_test.cpp
)

target_include_directories(ipc_fault_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/unit/ipc
  ${CMAKE_SOURCE_DIR}/modules/ipc/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ipc_fault_tests PRIVATE tcu_ipc gtest_main)

add_dependencies(ipc_fault_tests vehicle_service)
target_compile_definitions(ipc_fault_tests PRIVATE
  TCU_VEHICLE_SERVICE_BIN="$<TARGET_FILE:vehicle_service>")

include(GoogleTest)
gtest_discover_tests(ipc_fault_tests PROPERTIES LABELS "ipc-fault")
