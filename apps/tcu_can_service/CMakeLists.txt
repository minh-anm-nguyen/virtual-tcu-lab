# Project 05, spec section 21.4: the subject of the laboratory. Serves the
# section 16.2 health surface and the metrics snapshot over project 02's IPC.
add_executable(tcu_can_service main.cpp)
target_link_libraries(tcu_can_service PRIVATE tcu_can tcu_ipc tcu_logging tcu_eventbus)
