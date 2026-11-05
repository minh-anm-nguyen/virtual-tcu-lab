# Spec section 20, steps 3-4: an AT command timeout racing a reply, with the
# single-completion rule of spec section 12.13 owned by the caller.
add_executable(modem_timeout_demo main.cpp)
target_link_libraries(modem_timeout_demo PRIVATE tcu_scheduler)
