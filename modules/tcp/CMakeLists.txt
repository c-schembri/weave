add_library(weave_tcp STATIC src/windows/tcp.cpp)
weave_module(tcp)
target_link_libraries(weave_tcp PUBLIC weave::io PRIVATE ws2_32 mswsock)
target_include_directories(weave_tcp PRIVATE ../io/src)
target_compile_definitions(weave_tcp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
if(WEAVE_BUILD_TESTS)
  weave_add_test(weave_tcp_tests tests/tcp.cpp)
  target_link_libraries(weave_tcp_tests PRIVATE weave::tcp)
  target_include_directories(weave_tcp_tests PRIVATE ${PROJECT_SOURCE_DIR}/test_support ../io/src)
  target_compile_definitions(weave_tcp_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)

  weave_add_test(weave_tcp_serve_tests tests/serve.cpp tests/on_data.cpp)
  target_link_libraries(weave_tcp_serve_tests PRIVATE weave::tcp)
  target_include_directories(weave_tcp_serve_tests PRIVATE ${PROJECT_SOURCE_DIR}/test_support ../io/src)
  target_compile_definitions(weave_tcp_serve_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)

  add_executable(weave_tcp_contract_fixture tests/contract.cpp)
  target_link_libraries(weave_tcp_contract_fixture PRIVATE weave::tcp)
  weave_no_exceptions(weave_tcp_contract_fixture)
  add_test(NAME weave_tcp_contract_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --executable "$<TARGET_FILE:weave_tcp_contract_fixture>")
  set_tests_properties(weave_tcp_contract_tests PROPERTIES TIMEOUT 45)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_tcp_contract_tests PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()
endif()
