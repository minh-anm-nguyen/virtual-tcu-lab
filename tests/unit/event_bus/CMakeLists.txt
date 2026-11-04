include(GoogleTest)

# The TIMEOUT is mandatory, not cosmetic: a Drain stop waiting on a stuck
# handler hangs forever (contract G17), and a hang must fail loudly.
function(tcu_add_event_bus_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE tcu_eventbus GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 30)
endfunction()

# Further test files are registered by T039, T055 and T068.
tcu_add_event_bus_test(event_bus_test)
tcu_add_event_bus_test(queue_test)
tcu_add_event_bus_test(subscription_test)
tcu_add_event_bus_test(shutdown_test)
tcu_add_event_bus_test(callback_test)
tcu_add_event_bus_test(concurrency_test)

# T083: links the demo library so the shipped scenario is covered by a test.
add_executable(demo_test demo_test.cpp)
target_link_libraries(demo_test PRIVATE tcu_eventbus event_bus_demo_lib GTest::gtest_main)
gtest_discover_tests(demo_test PROPERTIES TIMEOUT 30)
