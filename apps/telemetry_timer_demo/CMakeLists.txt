# Spec section 20, steps 1-2 and 5-7: periodic telemetry sampling onto the
# project 01 Event Bus, both periodic modes, both stop modes.
add_executable(telemetry_timer_demo main.cpp)
target_link_libraries(telemetry_timer_demo PRIVATE tcu_scheduler tcu_eventbus)
