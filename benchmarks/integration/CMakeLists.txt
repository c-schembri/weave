add_executable(weave_concurrent concurrent.cpp)
weave_configure_benchmark(weave_concurrent)
target_link_libraries(weave_concurrent PRIVATE weave::tcp weave::runtime)
set_target_properties(weave_concurrent PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")

add_executable(weave_runtime_load ../support/runtime_load.cpp)
weave_configure_benchmark(weave_runtime_load)
set_target_properties(weave_runtime_load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")

if(WEAVE_BUILD_TESTS)
  add_test(NAME concurrent_smoke COMMAND weave_concurrent --benchmark_filter=connections:64/ --weave_duration_ms=100)
  set_tests_properties(concurrent_smoke PROPERTIES TIMEOUT 60 LABELS benchmark)
  add_test(NAME paired_smoke COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/paired_smoke.py" --binary "$<TARGET_FILE:weave_concurrent>")
  set_tests_properties(paired_smoke PROPERTIES TIMEOUT 60 LABELS benchmark)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(concurrent_smoke paired_smoke PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()
endif()
