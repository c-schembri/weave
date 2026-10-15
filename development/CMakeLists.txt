add_subdirectory(packaging)

if(WEAVE_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(benchmarks/tests)
  add_subdirectory("${PROJECT_SOURCE_DIR}/modules/runtime/benchmarks/tests"
    "${PROJECT_BINARY_DIR}/modules/runtime/benchmarks/tests")
endif()

if(WEAVE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
