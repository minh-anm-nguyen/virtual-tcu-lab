# PT-01 measurement. Reports p50/p95/p99 lateness; never gates on it
# (research R-18).
add_executable(scheduler_bench main.cpp)
target_link_libraries(scheduler_bench PRIVATE tcu_scheduler)
