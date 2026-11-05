# Project 04: libtcu_logging. Standard library ONLY - no project 01/02/03
# target appears here on purpose (specs/004-configuration-logging-service/
# tasks.md, dependency-direction note): every project from 01 to 30 must be
# able to link the logger without inheriting a timer or a transport.
add_library(tcu_logging STATIC
  src/json_formatter.cpp
  src/log_queue.cpp
  src/console_sink.cpp
  src/file_sink.cpp
  src/async_logger.cpp
)

target_include_directories(tcu_logging PUBLIC include)
target_compile_features(tcu_logging PUBLIC cxx_std_17)

# src/ holds private headers; they are reachable from the sources only.
target_include_directories(tcu_logging PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_logging PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(tcu_logging PUBLIC Threads::Threads)
