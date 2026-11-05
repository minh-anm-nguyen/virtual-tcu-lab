# Project 05: the synthetic ECUs. Periodic transmission on project 03's
# scheduler, runtime fault injection over project 02's IPC.
add_executable(vehicle_simulator main.cpp)
target_link_libraries(vehicle_simulator PRIVATE tcu_can tcu_ipc tcu_scheduler)
