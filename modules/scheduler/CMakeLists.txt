# Project 03: Timer & Task Scheduler. Portable - standard library only, so this
# target is NOT Linux-guarded (specs/003-timer-task-scheduler/research.md R-00).
add_library(tcu_scheduler STATIC
  src/clock_adapter.cpp
  src/metrics.cpp
  src/deadline_queue.cpp
  src/ready_queue.cpp
  src/log.cpp
  src/timer_scheduler.cpp
)

target_include_directories(tcu_scheduler PUBLIC include)
target_compile_features(tcu_scheduler PUBLIC cxx_std_17)

# src/ holds private headers; they are reachable from the sources only.
target_include_directories(tcu_scheduler PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_scheduler PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(tcu_scheduler PUBLIC Threads::Threads)
