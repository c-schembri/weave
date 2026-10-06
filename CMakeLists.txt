cmake_minimum_required(VERSION 3.25)
project(weave VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  set(CMAKE_VS_GLOBALS "VcpkgEnabled=false;VcpkgApplocalDeps=false")
endif()

set(WEAVE_MODULES "core;io;runtime;tcp;sync" CACHE STRING "Components to build (dependencies are included automatically)")
option(WEAVE_BUILD_TESTS "Build correctness tests" OFF)
option(WEAVE_BUILD_BENCHMARKS "Build networking comparison benchmarks" OFF)
set(WEAVE_BENCHMARK_SUITE "full" CACHE STRING "Benchmark suite: full or runtime (Weave/Asio TCP only)")
set_property(CACHE WEAVE_BENCHMARK_SUITE PROPERTY STRINGS full runtime)
if(NOT WEAVE_BENCHMARK_SUITE MATCHES "^(full|runtime)$")
  message(FATAL_ERROR "WEAVE_BENCHMARK_SUITE must be full or runtime")
endif()
option(WEAVE_BUILD_EXAMPLES "Build examples" OFF)
option(WEAVE_ENABLE_ASAN "Instrument Weave and consumers with AddressSanitizer (use Release)" OFF)
option(WEAVE_PROFILE_RUNTIME "Count coroutine allocations and I/O submissions (not for timing comparisons)" OFF)
option(WEAVE_TRACE_RUNTIME "Record diagnostic execution traces (not for timing comparisons)" OFF)
option(WEAVE_RECYCLE_FRAMES "Reuse coroutine frames in a bounded thread-local cache" ON)

if(WEAVE_BUILD_BENCHMARKS AND NOT WIN32)
  message(FATAL_ERROR "The native benchmark harness currently supports Windows only; Linux correctness builds are available.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/WeaveModule.cmake)

set(WEAVE_ENABLED_MODULES core)
foreach(module IN LISTS WEAVE_MODULES)
  if(NOT module MATCHES "^(core|io|runtime|tcp|sync|tls)$")
    message(FATAL_ERROR "Unknown Weave component: ${module}")
  endif()
  list(APPEND WEAVE_ENABLED_MODULES "${module}")
  if(module MATCHES "^(runtime|tcp|sync|tls)$")
    list(APPEND WEAVE_ENABLED_MODULES io)
  endif()
  if(module STREQUAL "tls")
    list(APPEND WEAVE_ENABLED_MODULES tcp sync)
  endif()
endforeach()
list(REMOVE_DUPLICATES WEAVE_ENABLED_MODULES)
if("io" IN_LIST WEAVE_ENABLED_MODULES AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Weave IO supports Windows IOCP and Linux io_uring. Core is portable.")
endif()

if(WEAVE_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  foreach(script IN ITEMS automation gate gate_process tokio_benchmark scaling_benchmark trace_runtime)
    add_test(NAME ${script}_tests COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/${script}.py")
    set_tests_properties(${script}_tests PROPERTIES TIMEOUT 60 LABELS tooling)
  endforeach()
  include(FetchContent)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5)
  FetchContent_MakeAvailable(doctest)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    get_filename_component(WEAVE_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  endif()
endif()

# Keep dependency order explicit; each module owns its sources, headers and tests.
foreach(module IN ITEMS core io runtime tcp sync tls)
  if(module IN_LIST WEAVE_ENABLED_MODULES)
    add_subdirectory(modules/${module})
  endif()
endforeach()

configure_package_config_file(cmake/weave-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/weave-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/weave-config-version.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/weave-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/weave-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")

set(WEAVE_FULL_TESTS OFF)
if(WEAVE_BUILD_TESTS AND TARGET weave_tcp AND TARGET weave_runtime)
  set(WEAVE_FULL_TESTS ON)
endif()
if(WEAVE_FULL_TESTS OR WEAVE_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG 366dfc44640182cb21c1ebf7efb658a6bec13f5a)
  FetchContent_MakeAvailable(asio)
endif()
if(WEAVE_FULL_TESTS)
  weave_add_test(weave_tests tests/integration/main.cpp tests/integration/runtime.cpp
    tests/integration/task.cpp tests/integration/task_concurrent.cpp tests/integration/cancellation.cpp
    tests/integration/serve.cpp tests/integration/endpoints.cpp)
  if(WIN32)
    target_sources(weave_tests PRIVATE tests/benchmark_affinity.cpp)
  endif()
  target_link_libraries(weave_tests PRIVATE weave::tcp weave::runtime)
  target_include_directories(weave_tests PRIVATE test_support)
  target_include_directories(weave_tests SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(weave_tests PRIVATE ASIO_STANDALONE ASIO_NO_EXCEPTIONS)
endif()

if(TARGET weave_tests)
  target_compile_definitions(weave_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
endif()
if(WEAVE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(WEAVE_BUILD_EXAMPLES)
  foreach(module IN LISTS WEAVE_ENABLED_MODULES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/${module}/examples/CMakeLists.txt")
      add_subdirectory(modules/${module}/examples)
    endif()
  endforeach()
endif()

if(WEAVE_BUILD_TESTS)
  set(package_cases ${WEAVE_ENABLED_MODULES})
  if("tcp" IN_LIST WEAVE_ENABLED_MODULES AND "runtime" IN_LIST WEAVE_ENABLED_MODULES)
    list(APPEND package_cases all)
  endif()
  foreach(component IN LISTS package_cases)
    add_test(NAME package_components_${component} COMMAND "${CMAKE_COMMAND}"
      "-DWEAVE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DWEAVE_WORK=${CMAKE_CURRENT_BINARY_DIR}/package-tests"
      "-DWEAVE_GENERATOR=${CMAKE_GENERATOR}"
      "-DWEAVE_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DWEAVE_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
      "-DWEAVE_CONFIG=$<CONFIG>"
      "-DWEAVE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DWEAVE_TEST_MODULES=${component}"
      "-DWEAVE_ASAN=${WEAVE_ENABLE_ASAN}"
      "-DWEAVE_PACKAGE_TLS=$<TARGET_EXISTS:weave_tls>"
      "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
      "-DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}"
      "-DWEAVE_TLS_RUNTIME_DIR=${WEAVE_TLS_RUNTIME_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package/components.cmake")
    set_tests_properties(package_components_${component} PROPERTIES TIMEOUT 180 LABELS packaging)
  endforeach()
endif()
