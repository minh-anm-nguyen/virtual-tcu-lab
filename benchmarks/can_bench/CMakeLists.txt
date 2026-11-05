# Project 05, spec section 18.4: receipt-to-decoded characterization.
#
# It asserts nothing and is not registered with CTest. NFR-05 states no latency
# target and section 18.4 forbids universal performance claims from one host, so
# a pass/fail gate here would be a claim the spec explicitly refuses to make.
add_executable(can_bench main.cpp)
target_link_libraries(can_bench PRIVATE tcu_can)
