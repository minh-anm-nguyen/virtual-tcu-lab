# Project 05: the synthetic body control module. 0x200 in, 0x201 and 0x103 out.
add_executable(bcm_simulator main.cpp)
target_link_libraries(bcm_simulator PRIVATE tcu_can)
