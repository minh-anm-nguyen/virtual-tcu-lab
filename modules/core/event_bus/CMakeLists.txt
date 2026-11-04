find_package(Threads REQUIRED)

add_library(tcu_eventbus STATIC
  src/bounded_queue.cpp
  src/subscription_registry.cpp
  src/event_bus.cpp
)

target_include_directories(tcu_eventbus PUBLIC include)
target_link_libraries(tcu_eventbus PUBLIC Threads::Threads)
