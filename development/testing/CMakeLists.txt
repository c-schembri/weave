if(WEAVE_FULL_TESTS)
  weave_add_test(weave_tests
    integration/main.cpp integration/runtime.cpp integration/task.cpp integration/task_concurrent.cpp
    integration/cancellation.cpp integration/serve.cpp integration/endpoints.cpp)
  if(WIN32)
    target_sources(weave_tests PRIVATE ${PROJECT_SOURCE_DIR}/development/benchmarks/tests/benchmark_affinity.cpp)
  endif()
  set_target_properties(weave_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  target_link_libraries(weave_tests PRIVATE weave::tcp weave::runtime)
  target_include_directories(weave_tests PRIVATE "${PROJECT_SOURCE_DIR}/development/testing/support")
  target_include_directories(weave_tests SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(weave_tests PRIVATE ASIO_STANDALONE ASIO_NO_EXCEPTIONS)
endif()

add_test(NAME source_layout_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/layout.py")
set_tests_properties(source_layout_tests PROPERTIES TIMEOUT 30 LABELS "tooling;layout")

if(TARGET weave_tests)
  target_compile_definitions(weave_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
endif()
