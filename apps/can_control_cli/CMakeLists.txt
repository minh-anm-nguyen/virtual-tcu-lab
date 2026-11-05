# Project 05: the operator console. A project 02 IPC client and nothing else.
add_executable(can_control_cli main.cpp)
target_link_libraries(can_control_cli PRIVATE tcu_ipc)
