# Project 02: IPC Service Framework. Linux only - see specs/002-.../research.md R-00.
add_library(tcu_ipc STATIC
  src/frame_codec.cpp
  src/unix_socket.cpp
  src/executor.cpp
  src/log.cpp
  src/client_session.cpp
  src/request_router.cpp
  src/ipc_client.cpp
  src/ipc_server.cpp
)

target_include_directories(tcu_ipc PUBLIC include)
target_compile_features(tcu_ipc PUBLIC cxx_std_17)

# src/ holds private headers; they are reachable from the sources only.
target_include_directories(tcu_ipc PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_ipc PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(tcu_ipc PUBLIC Threads::Threads)
