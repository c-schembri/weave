add_executable(weave_echo echo.cpp)
target_link_libraries(weave_echo PRIVATE weave::tcp)
weave_no_exceptions(weave_echo)
set_target_properties(weave_echo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
if(WEAVE_BUILD_BENCHMARKS AND TARGET weave_runtime)
  # The equivalent concurrent Weave server uses Runtime; all four reuse benchmark dependencies.
  add_subdirectory(echo_servers)
endif()
