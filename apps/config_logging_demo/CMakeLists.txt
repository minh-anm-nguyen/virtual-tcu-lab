# Spec section 21: the six-scenario demonstration - valid startup, valid hot
# reload, rejected reload, restart-only rejection, log flood, sink error.
add_executable(config_logging_demo main.cpp)
target_link_libraries(config_logging_demo
  PRIVATE tcu_config tcu_logging tcu_scheduler tcu_eventbus)
target_compile_definitions(config_logging_demo
  PRIVATE TCU_CONFIGS_DIR="${CMAKE_SOURCE_DIR}/configs")
