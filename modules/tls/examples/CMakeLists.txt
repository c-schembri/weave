add_executable(tls_echo_server echo/server.cpp)
target_link_libraries(tls_echo_server PRIVATE weave::tls)
weave_no_exceptions(tls_echo_server)
weave_tls_runtime(tls_echo_server)
set_target_properties(tls_echo_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
if(WEAVE_BUILD_TESTS)
  add_test(NAME weave_tls_echo_example COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/interop.py"
    --executable "$<TARGET_FILE:weave_tls_interop>" --echo-example "$<TARGET_FILE:tls_echo_server>")
  set_tests_properties(weave_tls_echo_example PROPERTIES TIMEOUT 45 LABELS examples)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_tls_echo_example PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()
endif()
