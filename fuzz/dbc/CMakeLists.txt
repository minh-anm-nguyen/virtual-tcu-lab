# libFuzzer targets. Built only when TCU_BUILD_FUZZERS=ON and only with clang,
# because libFuzzer provides its own main and needs -fsanitize=fuzzer
# (research R-21). They are deliberately NOT ctest cases: the budget is a
# script, so a default build stays green on GCC and on Windows.

set(TCU_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

foreach(target fuzz_dbc_parser fuzz_frame_decoder)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE tcu_dbc)
  target_compile_options(${target} PRIVATE ${TCU_FUZZ_FLAGS})
  target_link_options(${target} PRIVATE ${TCU_FUZZ_FLAGS})
endforeach()
