# A library so the smoke test can run the same scenario in-process (T083).
add_library(event_bus_demo_lib STATIC demo.cpp)
target_include_directories(event_bus_demo_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(event_bus_demo_lib PUBLIC tcu_eventbus)

add_executable(event_bus_demo main.cpp)
target_link_libraries(event_bus_demo PRIVATE event_bus_demo_lib)
