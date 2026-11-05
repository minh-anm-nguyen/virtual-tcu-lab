# Project 04: libtcu_config. Links tcu_logging and tcu_scheduler PUBLIC because
# both appear in its public headers (research R-02: the scheduler is the
# notifier executor and the file poller). nlohmann/json is PRIVATE - it is
# wrapped away by src/json_document.hpp, so no consumer inherits it (R-01).
add_library(tcu_config STATIC
  src/json_document.cpp
  src/loader.cpp
  src/validator.cpp
  src/file_poller.cpp
  src/configuration_service.cpp
)

target_include_directories(tcu_config PUBLIC include)
target_compile_features(tcu_config PUBLIC cxx_std_17)
target_include_directories(tcu_config PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tcu_config PRIVATE -Wall -Wextra)
endif()

target_link_libraries(tcu_config
  PUBLIC tcu_logging tcu_scheduler
  PRIVATE nlohmann_json::nlohmann_json
)
