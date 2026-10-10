cmake_minimum_required(VERSION 3.25)

function(run)
  cmake_parse_arguments(PARSE_ARGV 0 command "" "" "")
  execute_process(COMMAND ${command_UNPARSED_ARGUMENTS}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${error}")
  endif()
endfunction()

set(generator -G "${WEAVE_GENERATOR}")
if(WEAVE_PLATFORM)
  list(APPEND generator -A "${WEAVE_PLATFORM}")
endif()
if(WEAVE_TOOLSET)
  list(APPEND generator -T "${WEAVE_TOOLSET}")
endif()
if(NOT WEAVE_CONFIG)
  set(WEAVE_CONFIG Release)
endif()
if(WEAVE_ASAN AND WIN32)
  get_filename_component(compiler_bin "${WEAVE_CXX_COMPILER}" DIRECTORY)
  set(ENV{PATH} "${compiler_bin};$ENV{PATH}")
endif()
if(WEAVE_TLS_RUNTIME_DIR AND WIN32)
  set(ENV{PATH} "${WEAVE_TLS_RUNTIME_DIR};$ENV{PATH}")
endif()
if(ICU_ROOT AND WIN32)
  set(ENV{PATH} "${ICU_ROOT}/bin;$ENV{PATH}")
endif()

set(openssl_options "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
  "-DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}" "-DICU_ROOT=${ICU_ROOT}")
if(NOT "${WEAVE_POSTGRES_GSSAPI}" STREQUAL "")
  list(APPEND openssl_options "-DWEAVE_POSTGRES_GSSAPI=${WEAVE_POSTGRES_GSSAPI}")
endif()
if(NOT "${WEAVE_POSTGRES_LDAP}" STREQUAL "")
  list(APPEND openssl_options "-DWEAVE_POSTGRES_LDAP=${WEAVE_POSTGRES_LDAP}")
endif()
if(WeaveLDAP_INCLUDE_DIR AND WeaveLDAP_LIBRARY)
  list(APPEND openssl_options "-DWeaveLDAP_INCLUDE_DIR=${WeaveLDAP_INCLUDE_DIR}" "-DWeaveLDAP_LIBRARY=${WeaveLDAP_LIBRARY}")
endif()

set(cases ${WEAVE_TEST_MODULES})
if("tcp" IN_LIST WEAVE_TEST_MODULES AND "runtime" IN_LIST WEAVE_TEST_MODULES)
  list(APPEND cases all)
endif()
foreach(component IN LISTS cases)
  set(work "${WEAVE_WORK}/${WEAVE_CONFIG}/${component}")
  set(expected core)
  if(NOT component STREQUAL "core")
    list(APPEND expected io)
  endif()
  if(component STREQUAL "tcp" OR component STREQUAL "local" OR component STREQUAL "runtime" OR component STREQUAL "sync")
    list(APPEND expected "${component}")
  endif()
  if(component STREQUAL "tls" OR component STREQUAL "postgres")
    list(APPEND expected tcp sync tls)
  endif()
  if(component STREQUAL "postgres")
    list(APPEND expected local postgres)
  endif()
  set(roots "${component}")
  set(requested "${component}")
  set(installed "${expected}")
  if(component STREQUAL "all")
    set(roots "tcp;runtime")
    set(requested tcp)
    set(expected "core;io;tcp")
    set(installed "core;io;tcp;runtime")
    if(WEAVE_PACKAGE_TLS)
      list(APPEND roots sync tls)
      list(APPEND installed sync tls)
    else()
      list(APPEND roots sync)
      list(APPEND installed sync)
    endif()
    if(WEAVE_PACKAGE_POSTGRES)
      list(APPEND roots postgres)
      list(APPEND installed local postgres)
    endif()
    if(WEAVE_PACKAGE_LOCAL)
      list(APPEND roots local)
      list(APPEND installed local)
    endif()
    list(REMOVE_DUPLICATES installed)
  endif()

  message(STATUS "Package isolation: ${component}")
  run("${CMAKE_COMMAND}" -S "${WEAVE_SOURCE}" -B "${work}/library" ${generator}
    "-DCMAKE_CXX_COMPILER=${WEAVE_CXX_COMPILER}" "-DCMAKE_BUILD_TYPE=${WEAVE_CONFIG}"
    "-DWEAVE_MODULES=${roots}" "-DWEAVE_BUILD_TESTS=OFF" "-DWEAVE_BUILD_BENCHMARKS=OFF"
    "-DWEAVE_BUILD_EXAMPLES=OFF" "-DWEAVE_ENABLE_ASAN=${WEAVE_ASAN}" "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    ${openssl_options})
  if(EXISTS "${work}/library/_deps")
    message(FATAL_ERROR "Library-only configuration must not populate third-party dependencies")
  endif()
  run("${CMAKE_COMMAND}" --build "${work}/library" --config "${WEAVE_CONFIG}" --parallel 4)
  run("${CMAKE_COMMAND}" --install "${work}/library" --config "${WEAVE_CONFIG}" --prefix "${work}/install")
  # Consume a relocated copy, never the source/build tree's include directories.
  file(COPY "${work}/install/" DESTINATION "${work}/relocated")
  run("${CMAKE_COMMAND}" -S "${WEAVE_SOURCE}/tests/package/consumer" -B "${work}/consumer" ${generator}
    "-DCMAKE_CXX_COMPILER=${WEAVE_CXX_COMPILER}" "-DCMAKE_BUILD_TYPE=${WEAVE_CONFIG}"
    "-Dweave_DIR=${work}/relocated/lib/cmake/weave" "-DWEAVE_COMPONENT=${requested}"
    "-DWEAVE_EXPECTED_MODULES=${expected}" "-DWEAVE_INSTALLED_MODULES=${installed}"
    "-DWEAVE_PREFIX=${work}/relocated" ${openssl_options})
  run("${CMAKE_COMMAND}" --build "${work}/consumer" --config "${WEAVE_CONFIG}" --parallel 4)
  run("${CMAKE_CTEST_COMMAND}" --test-dir "${work}/consumer" -C "${WEAVE_CONFIG}" --output-on-failure --no-tests=error)

  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND WEAVE_POSTGRES_LDAP AND
      (component STREQUAL "postgres" OR (component STREQUAL "all" AND WEAVE_PACKAGE_POSTGRES)))
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WEAVE_SOURCE}/tests/package/consumer"
      -B "${work}/missing-ldap" ${generator}
      "-DCMAKE_CXX_COMPILER=${WEAVE_CXX_COMPILER}" "-Dweave_DIR=${work}/relocated/lib/cmake/weave"
      "-DWEAVE_COMPONENT=postgres" "-DWEAVE_FIND_ONLY=ON" "-DCMAKE_DISABLE_FIND_PACKAGE_WeaveLDAP=TRUE"
      ${openssl_options} RESULT_VARIABLE missing_ldap OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(missing_ldap STREQUAL "0" OR NOT "${output}\n${error}" MATCHES "LDAP support requires OpenLDAP")
      message(FATAL_ERROR "Missing LDAP dependency was not diagnosed correctly: ${output}\n${error}")
    endif()
  endif()

  # Missing required components must fail instead of silently loading everything.
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WEAVE_SOURCE}/tests/package/consumer"
    -B "${work}/missing" ${generator}
    "-DCMAKE_CXX_COMPILER=${WEAVE_CXX_COMPILER}" "-Dweave_DIR=${work}/relocated/lib/cmake/weave"
    "-DWEAVE_COMPONENT=not_installed" "-DWEAVE_FIND_ONLY=ON"
    RESULT_VARIABLE missing OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
  if(missing STREQUAL "0" OR NOT "${output}\n${error}" MATCHES "not_installed.*not installed")
    message(FATAL_ERROR "Missing required component was not diagnosed correctly: ${output}\n${error}")
  endif()
endforeach()
