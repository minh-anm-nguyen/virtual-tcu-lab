# Project 06: libtcu_dbc. Links NOTHING - not even tcu_logging. A parser that
# can be fuzzed and property-tested with no lifecycle at all is the reason the
# project is split in two (plan.md Structure Decision); diagnostics are
# returned, never logged, from here.
#
# Portable: no POSIX call, so this target sits outside the root Linux guard
# (research R-00).
add_library(tcu_dbc STATIC
  src/dbc_types.cpp
  src/sha256.cpp
  src/dbc_lexer.cpp
  src/dbc_parser_impl.cpp
  src/schema_validator.cpp
  src/compiled_bitplan.cpp
  src/dbc_load.cpp
)

target_include_directories(tcu_dbc PUBLIC include PRIVATE src)
target_compile_features(tcu_dbc PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # -Wconversion is asked for by spec section 23's Definition of Done.
  target_compile_options(tcu_dbc PRIVATE -Wall -Wextra -Wconversion)
endif()
