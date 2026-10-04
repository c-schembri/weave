foreach(library IN ITEMS weave asio libuv usockets)
  add_executable(echo_${library} ${library}.cpp)
  set_target_properties(echo_${library} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples/echo_servers/$<CONFIG>")
  target_compile_features(echo_${library} PRIVATE cxx_std_23)
  target_compile_definitions(echo_${library} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
  weave_no_exceptions(echo_${library})
  if(WEAVE_ENABLE_ASAN AND MSVC)
    target_compile_options(echo_${library} PRIVATE /fsanitize=address /Zi)
    target_link_options(echo_${library} PRIVATE /INCREMENTAL:NO /DEBUG)
  endif()
  if(WEAVE_BUILD_TESTS)
    add_test(NAME echo_${library}_integration
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/echo_example.ps1 -Executable $<TARGET_FILE:echo_${library}>)
    set_tests_properties(echo_${library}_integration PROPERTIES TIMEOUT 45 LABELS examples)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(echo_${library}_integration PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
  endif()
endforeach()

target_link_libraries(echo_weave PRIVATE weave::tcp weave::runtime)
target_include_directories(echo_asio SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(echo_asio PRIVATE ASIO_STANDALONE ASIO_NO_EXCEPTIONS)
target_link_libraries(echo_asio PRIVATE ws2_32 mswsock)
target_link_libraries(echo_libuv PRIVATE uv_a)
target_link_libraries(echo_usockets PRIVATE weave_usockets)
