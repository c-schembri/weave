# Existing coroutine comparisons use Context as their driver. Keep that harness
# unchanged instead of silently changing the measurements in a directory move.
if(NOT TARGET weave_io)
  message(FATAL_ERROR "The comparison harness requires io in WEAVE_MODULES. The core library and tests do not.")
endif()

function(weave_configure_network_comparison target)
  target_link_libraries(${target} PRIVATE weave::core ws2_32 mswsock)
  target_include_directories(${target} PRIVATE
    "${PROJECT_SOURCE_DIR}/benchmarks/support" "${PROJECT_SOURCE_DIR}/test_support")
  target_include_directories(${target} SYSTEM PRIVATE "${asio_SOURCE_DIR}/asio/include")
  target_compile_definitions(${target} PRIVATE ASIO_STANDALONE ASIO_NO_EXCEPTIONS
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
  weave_no_exceptions(${target})
endfunction()

if(WEAVE_BENCHMARK_SUITE STREQUAL "runtime")
  if(NOT TARGET weave_tcp OR NOT TARGET weave_runtime)
    message(FATAL_ERROR "The runtime comparison requires tcp and runtime in WEAVE_MODULES")
  endif()
  add_subdirectory("${PROJECT_SOURCE_DIR}/modules/tcp/benchmarks" "${PROJECT_BINARY_DIR}/modules/tcp/benchmarks")
  add_executable(weave_runtime_load support/runtime_load.cpp)
  weave_configure_network_comparison(weave_runtime_load)
  set_target_properties(weave_runtime_load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
  return()
endif()

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG 31c66f47f60d06790a4885a261d682ba339bd100)
FetchContent_MakeAvailable(benchmark)
if(WEAVE_ENABLE_ASAN AND MSVC)
  target_compile_options(benchmark PUBLIC /fsanitize=address /Zi)
endif()

if(TARGET weave_tcp OR TARGET weave_runtime)
  enable_language(C)
  set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libuv GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG 840404ce8ba7cc0204be52389a6cfff9f2c90fb6 GIT_SUBMODULES "")
  FetchContent_Declare(usockets GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG 7a7c820db4740c2a2a4faf0d918c4eec2ac1fac3 GIT_SUBMODULES "")
  FetchContent_MakeAvailable(libuv usockets)
  # Upstream uSockets ships a Makefile, not CMake. Use its plain TCP/libuv sources.
  add_library(weave_usockets STATIC
    ${usockets_SOURCE_DIR}/src/bsd.c
    ${usockets_SOURCE_DIR}/src/context.c
    ${usockets_SOURCE_DIR}/src/loop.c
    ${usockets_SOURCE_DIR}/src/socket.c
    ${usockets_SOURCE_DIR}/src/udp.c
    ${usockets_SOURCE_DIR}/src/eventing/libuv.c)
  target_compile_features(weave_usockets PRIVATE c_std_11)
  target_include_directories(weave_usockets SYSTEM PUBLIC ${usockets_SOURCE_DIR}/src)
  target_compile_definitions(weave_usockets PUBLIC LIBUS_USE_LIBUV LIBUS_NO_SSL)
  target_link_libraries(weave_usockets PUBLIC uv_a)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    target_compile_options(uv_a PRIVATE /fsanitize=address /Zi)
    target_compile_options(weave_usockets PRIVATE /fsanitize=address /Zi)
  endif()
endif()

function(weave_configure_benchmark target)
  weave_configure_network_comparison(${target})
  target_link_libraries(${target} PRIVATE benchmark::benchmark)
endfunction()

function(weave_benchmark_group module)
  set(target weave_bench_${module})
  # Object libraries retain every static benchmark registration in the executable.
  add_library(${target} OBJECT ${ARGN})
  weave_configure_benchmark(${target})
  target_link_libraries(weave_bench PRIVATE ${target})
endfunction()

add_executable(weave_bench support/main.cpp)
weave_configure_benchmark(weave_bench)
set_target_properties(weave_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
if(TARGET weave_usockets)
  target_link_libraries(weave_bench PRIVATE weave_usockets)
  target_compile_definitions(weave_bench PRIVATE WEAVE_BENCH_NATIVE=1)
endif()
foreach(module IN LISTS WEAVE_ENABLED_MODULES)
  if(EXISTS "${PROJECT_SOURCE_DIR}/modules/${module}/benchmarks/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/modules/${module}/benchmarks"
      "${PROJECT_BINARY_DIR}/modules/${module}/benchmarks")
  endif()
endforeach()

if(TARGET weave_tcp AND TARGET weave_runtime)
  add_subdirectory(integration)
endif()

if(WEAVE_BUILD_TESTS)
  add_test(NAME benchmark_smoke COMMAND weave_bench --benchmark_min_time=0.001s)
  set_tests_properties(benchmark_smoke PROPERTIES TIMEOUT 120 LABELS benchmark)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(benchmark_smoke PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()
  if(TARGET weave_tcp)
    add_executable(weave_benchmark_fixtures
      ${PROJECT_SOURCE_DIR}/tests/benchmark_fixtures.cpp ${PROJECT_SOURCE_DIR}/tests/native_clients.cpp)
    target_link_libraries(weave_benchmark_fixtures PRIVATE weave::tcp doctest::doctest weave_usockets)
    target_include_directories(weave_benchmark_fixtures PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_include_directories(weave_benchmark_fixtures SYSTEM PRIVATE "${asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(weave_benchmark_fixtures PRIVATE ASIO_STANDALONE ASIO_NO_EXCEPTIONS
      DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
    weave_no_exceptions(weave_benchmark_fixtures)
    set_target_properties(weave_benchmark_fixtures PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
    add_test(NAME benchmark_fixtures COMMAND weave_benchmark_fixtures)
    set_tests_properties(benchmark_fixtures PROPERTIES TIMEOUT 45 LABELS benchmark)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(benchmark_fixtures PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
  endif()
endif()
