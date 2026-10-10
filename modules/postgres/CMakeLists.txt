find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)
find_package(ICU 70 REQUIRED COMPONENTS uc i18n)
add_library(weave_postgres src/wire.cpp src/diagnostic.cpp src/result_format.cpp src/auth.cpp src/options.cpp src/configuration.cpp src/configuration_info.cpp src/encoding.cpp src/connection.cpp src/blocking.cpp
  src/large_object.cpp src/large_object_file.cpp src/gss.cpp src/gss_pool.cpp src/gss_stream.cpp src/oauth.cpp src/oauth_sasl.cpp
  src/oauth_json.cpp src/oauth_url.cpp src/oauth_http.cpp src/oauth_device.cpp src/events.cpp src/ldap.cpp)
weave_module(postgres)
set(WEAVE_POSTGRES_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}" CACHE PATH
  "Compiled-in PostgreSQL service directory; empty disables automatic system-file discovery")
function(weave_postgres_configuration_paths directory output)
  if(NOT directory STREQUAL "" AND NOT IS_ABSOLUTE "${directory}")
    message(FATAL_ERROR "WEAVE_POSTGRES_SYSCONFDIR must be absolute or empty")
  endif()
  string(LENGTH "${directory}" length)
  if(length GREATER 65536)
    message(FATAL_ERROR "WEAVE_POSTGRES_SYSCONFDIR exceeds the configuration-field limit")
  endif()
  # Encode UTF8 bytes rather than injecting a path into compiler definitions or source literals.
  string(HEX "${directory}" hex)
  string(REGEX MATCHALL ".." bytes "${hex}")
  set(weave_postgres_sysconf_literal "")
  foreach(byte IN LISTS bytes)
    string(APPEND weave_postgres_sysconf_literal "\\x${byte}")
  endforeach()
  configure_file(src/configuration_paths.hpp.in "${output}" @ONLY)
endfunction()
weave_postgres_configuration_paths("${WEAVE_POSTGRES_SYSCONFDIR}" private/configuration_paths.hpp)
target_include_directories(weave_postgres PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/private")

option(WEAVE_POSTGRES_LDAP "Build optional native PostgreSQL LDAP service lookup" OFF)
if(WEAVE_POSTGRES_LDAP)
  target_compile_definitions(weave_postgres PRIVATE WEAVE_POSTGRES_LDAP=1)
  if(WIN32)
    target_link_libraries(weave_postgres PRIVATE wldap32)
  else()
    list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
    find_package(WeaveLDAP REQUIRED)
    list(REMOVE_AT CMAKE_MODULE_PATH 0)
    target_link_libraries(weave_postgres PRIVATE WeaveLDAP::LDAP)
    install(FILES "${PROJECT_SOURCE_DIR}/cmake/FindWeaveLDAP.cmake"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")
  endif()
endif()
install(FILES src/vendor/yyjson/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/weave/licenses" RENAME yyjson-LICENSE)
install(FILES src/vendor/llhttp/LICENSE-MIT DESTINATION "${CMAKE_INSTALL_DATADIR}/weave/licenses" RENAME llhttp-LICENSE)
install(FILES src/vendor/uriparser/COPYING.BSD-3-Clause DESTINATION "${CMAKE_INSTALL_DATADIR}/weave/licenses"
  RENAME uriparser-LICENSE)

# Private C parsers are embedded in the PostgreSQL archive, never exported as consumer dependencies.
enable_language(C)
function(weave_postgres_parser target prefix)
  target_compile_features(${target} PRIVATE c_std_11)
  if(MSVC)
    target_compile_options(${target} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/${prefix}" /W3)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/${prefix}" -fvisibility=hidden)
  endif()
  if(WEAVE_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endif()
  endif()
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_sources(weave_postgres PRIVATE $<TARGET_OBJECTS:${target}>)
endfunction()

add_library(weave_postgres_http_parser OBJECT src/vendor/llhttp/src/api.c src/vendor/llhttp/src/http.c
  src/vendor/llhttp/src/llhttp.c)
target_include_directories(weave_postgres_http_parser PRIVATE src/vendor/llhttp/include)
weave_postgres_parser(weave_postgres_http_parser oauth_llhttp_prefix.h)

function(weave_postgres_uri_config)
  set(PROJECT_VERSION 1.0.2)
  set(HAVE_WPRINTF OFF)
  set(HAVE_REALLOCARRAY OFF)
  configure_file(src/vendor/uriparser/src/UriConfig.h.in uri/UriConfig.h)
endfunction()
weave_postgres_uri_config()
add_library(weave_postgres_uri_parser OBJECT src/vendor/uriparser/src/UriCommon.c src/vendor/uriparser/src/UriMemory.c
  src/vendor/uriparser/src/UriParseBase.c src/vendor/uriparser/src/UriParse.c
  src/vendor/uriparser/src/UriIp4Base.c src/vendor/uriparser/src/UriIp4.c)
target_include_directories(weave_postgres_uri_parser PRIVATE src/vendor/uriparser/include "${CMAKE_CURRENT_BINARY_DIR}/uri")
weave_postgres_parser(weave_postgres_uri_parser oauth_uri_prefix.h)
target_include_directories(weave_postgres PRIVATE src/vendor/uriparser/include)

target_link_libraries(weave_postgres PUBLIC weave::tls PRIVATE weave::local OpenSSL::Crypto ICU::uc ICU::i18n)
if(WIN32)
  target_compile_definitions(weave_postgres PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_sources(weave_postgres PRIVATE src/windows/gss.cpp src/windows/gss_pool.cpp src/windows/ldap.cpp)
  target_link_libraries(weave_postgres PRIVATE advapi32 secur32)
else()
  target_sources(weave_postgres PRIVATE src/linux/gss.cpp src/linux/gss_pool.cpp src/linux/ldap.cpp)
  option(WEAVE_POSTGRES_GSSAPI "Build native PostgreSQL GSSAPI authentication support" ON)
  if(WEAVE_POSTGRES_GSSAPI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEAVE_GSSAPI REQUIRED IMPORTED_TARGET krb5-gssapi)
    pkg_check_modules(WEAVE_KERBEROS REQUIRED IMPORTED_TARGET krb5)
    target_link_libraries(weave_postgres PRIVATE PkgConfig::WEAVE_GSSAPI PkgConfig::WEAVE_KERBEROS)
    target_compile_definitions(weave_postgres PRIVATE WEAVE_POSTGRES_GSSAPI=1)
  endif()
endif()

get_filename_component(icu_prefix "${ICU_INCLUDE_DIRS}" DIRECTORY)
set(WEAVE_POSTGRES_RUNTIME_DIR "${icu_prefix}/bin" CACHE PATH "Directory containing shared ICU runtime DLLs")
function(weave_postgres_runtime target)
  weave_tls_runtime(${target})
  if(WIN32 AND EXISTS "${WEAVE_POSTGRES_RUNTIME_DIR}")
    file(GLOB dlls "${WEAVE_POSTGRES_RUNTIME_DIR}/icu*.dll")
    foreach(dll IN LISTS dlls)
      add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dll}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
  endif()
endfunction()

if(WEAVE_BUILD_TESTS)
  add_executable(weave_postgres_tls_modes tests/tls_modes.cpp)
  target_include_directories(weave_postgres_tls_modes PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_tls_modes PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(tls_modes_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_tls_modes PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_tls_modes PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(tls_modes_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_tls_modes)
  weave_postgres_runtime(weave_postgres_tls_modes)
  weave_tls_runtime(weave_postgres_tls_modes)
  add_test(NAME weave_postgres_tls_modes COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_modes.py" --executable "$<TARGET_FILE:weave_postgres_tls_modes>" ${tls_modes_runtime})
  set_tests_properties(weave_postgres_tls_modes PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_tls_modes PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  if(WIN32 OR WEAVE_POSTGRES_GSSAPI)
    add_executable(weave_postgres_tls_reconnect tests/tls_reconnect.cpp)
    target_include_directories(weave_postgres_tls_reconnect PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_tls_reconnect PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET weave_runtime)
      target_link_libraries(weave_postgres_tls_reconnect PRIVATE weave::runtime)
      target_compile_definitions(weave_postgres_tls_reconnect PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endif()
    weave_no_exceptions(weave_postgres_tls_reconnect)
    weave_postgres_runtime(weave_postgres_tls_reconnect)
    add_test(NAME weave_postgres_tls_reconnect COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_reconnect.py"
      --executable "$<TARGET_FILE:weave_postgres_tls_reconnect>")
    set_tests_properties(weave_postgres_tls_reconnect PROPERTIES TIMEOUT 120 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_tls_reconnect PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
  endif()

  set(storage_runtime)
  foreach(suite result_storage result_storage_contract result_storage_guards result_storage_stream
      result_storage_pipeline result_storage_live result_storage_paths)
    set(target weave_postgres_${suite})
    add_executable(${target} tests/${suite}.cpp)
    target_link_libraries(${target} PRIVATE weave::postgres OpenSSL::Crypto)
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  foreach(suite result_storage result_storage_guards result_storage_pipeline)
    target_include_directories(weave_postgres_${suite} PRIVATE src)
  endforeach()
  foreach(suite result_storage_live result_storage_paths)
    target_include_directories(weave_postgres_${suite} PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  endforeach()
  if(TARGET weave_runtime)
    foreach(suite result_storage_stream result_storage_pipeline result_storage_live result_storage_paths)
      target_link_libraries(weave_postgres_${suite} PRIVATE weave::runtime)
      target_compile_definitions(weave_postgres_${suite} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endforeach()
    set(storage_runtime --runtime)
  endif()
  foreach(suite result_storage result_storage_contract result_storage_pipeline)
    add_test(NAME weave_postgres_${suite} COMMAND weave_postgres_${suite})
    set_tests_properties(weave_postgres_${suite} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(suite result_storage_guards result_storage_stream)
    set(arguments)
    if(suite STREQUAL "result_storage_stream")
      set(arguments ${storage_runtime})
    endif()
    add_test(NAME weave_postgres_${suite} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.py"
      --executable "$<TARGET_FILE:weave_postgres_${suite}>" ${arguments})
    set_tests_properties(weave_postgres_${suite} PROPERTIES TIMEOUT 120)
  endforeach()
  if(WEAVE_ENABLE_ASAN AND MSVC)
    foreach(suite result_storage result_storage_contract result_storage_guards result_storage_stream result_storage_pipeline)
      set_tests_properties(weave_postgres_${suite} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endforeach()
  endif()

  add_executable(weave_postgres_results tests/results.cpp)
  target_link_libraries(weave_postgres_results PRIVATE weave::postgres)
  add_executable(weave_postgres_results_live tests/results_live.cpp)
  target_include_directories(weave_postgres_results_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_results_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(result_runtime)
  if(TARGET weave_runtime)
    foreach(target weave_postgres_results weave_postgres_results_live)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endforeach()
    set(result_runtime --runtime)
  endif()
  foreach(target weave_postgres_results weave_postgres_results_live)
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_results COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/results.py"
    --executable "$<TARGET_FILE:weave_postgres_results>" ${result_runtime})
  set_tests_properties(weave_postgres_results PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_results PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_transaction tests/transaction.cpp)
  target_link_libraries(weave_postgres_transaction PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_transaction PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_transaction PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_transaction)
  weave_postgres_runtime(weave_postgres_transaction)
  add_test(NAME weave_postgres_transaction COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/transaction.py"
    --executable "$<TARGET_FILE:weave_postgres_transaction>" ${result_runtime})
  set_tests_properties(weave_postgres_transaction PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_transaction PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_pipeline_status tests/pipeline_status.cpp)
  target_link_libraries(weave_postgres_pipeline_status PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_pipeline_status PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_pipeline_status PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_pipeline_status)
  weave_postgres_runtime(weave_postgres_pipeline_status)
  add_test(NAME weave_postgres_pipeline_status COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_status.py"
    --executable "$<TARGET_FILE:weave_postgres_pipeline_status>" ${result_runtime})
  set_tests_properties(weave_postgres_pipeline_status PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_pipeline_status PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_flush_request tests/flush_request.cpp)
  add_executable(weave_postgres_flush_request_live tests/flush_request.cpp)
  target_compile_definitions(weave_postgres_flush_request_live PRIVATE WEAVE_POSTGRES_TEST_LIVE=1)
  target_include_directories(weave_postgres_flush_request_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_flush_request_live PRIVATE OpenSSL::Crypto)
  foreach(target weave_postgres_flush_request weave_postgres_flush_request_live)
    target_link_libraries(${target} PRIVATE weave::postgres)
    if(TARGET weave_runtime)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endif()
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_flush_request COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/flush_request.py"
    --executable "$<TARGET_FILE:weave_postgres_flush_request>" ${result_runtime})
  set_tests_properties(weave_postgres_flush_request PROPERTIES TIMEOUT 180 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_flush_request PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_exchange_chunks tests/exchange_chunks.cpp)
  add_executable(weave_postgres_exchange_chunks_live tests/exchange_chunks.cpp)
  target_compile_definitions(weave_postgres_exchange_chunks_live PRIVATE WEAVE_POSTGRES_TEST_LIVE=1)
  target_include_directories(weave_postgres_exchange_chunks_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_exchange_chunks_live PRIVATE OpenSSL::Crypto)
  foreach(target weave_postgres_exchange_chunks weave_postgres_exchange_chunks_live)
    target_link_libraries(${target} PRIVATE weave::postgres)
    if(TARGET weave_runtime)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endif()
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_exchange_chunks COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/exchange_chunks.py"
    --executable "$<TARGET_FILE:weave_postgres_exchange_chunks>" ${result_runtime})
  set_tests_properties(weave_postgres_exchange_chunks PROPERTIES TIMEOUT 180 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_exchange_chunks PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_failures tests/failures.cpp)
  target_link_libraries(weave_postgres_failures PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_failures PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_failures PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_failures)
  weave_postgres_runtime(weave_postgres_failures)
  add_test(NAME weave_postgres_failures COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/failures.py"
    --executable "$<TARGET_FILE:weave_postgres_failures>" ${result_runtime})
  set_tests_properties(weave_postgres_failures PROPERTIES TIMEOUT 240 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_failures PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_authentication_metadata tests/authentication_metadata.cpp)
  target_link_libraries(weave_postgres_authentication_metadata PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_authentication_metadata PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_authentication_metadata PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_authentication_metadata)
  weave_postgres_runtime(weave_postgres_authentication_metadata)
  add_test(NAME weave_postgres_authentication_metadata COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/authentication_metadata.py"
    --executable "$<TARGET_FILE:weave_postgres_authentication_metadata>" ${result_runtime})
  set_tests_properties(weave_postgres_authentication_metadata PROPERTIES TIMEOUT 240 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_authentication_metadata PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_session_metadata tests/session_metadata.cpp)
  target_link_libraries(weave_postgres_session_metadata PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_session_metadata PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_session_metadata PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_session_metadata)
  weave_postgres_runtime(weave_postgres_session_metadata)
  add_test(NAME weave_postgres_session_metadata COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_metadata.py"
    --executable "$<TARGET_FILE:weave_postgres_session_metadata>" ${result_runtime})
  set_tests_properties(weave_postgres_session_metadata PROPERTIES TIMEOUT 240 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_session_metadata PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_target_diagnostic tests/target_diagnostic.cpp)
  target_link_libraries(weave_postgres_target_diagnostic PRIVATE weave::postgres)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_target_diagnostic PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_target_diagnostic PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_target_diagnostic)
  weave_postgres_runtime(weave_postgres_target_diagnostic)
  add_test(NAME weave_postgres_target_diagnostic COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_diagnostic.py"
    --executable "$<TARGET_FILE:weave_postgres_target_diagnostic>" ${result_runtime})
  set_tests_properties(weave_postgres_target_diagnostic PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_target_diagnostic PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  option(WEAVE_POSTGRES_RESULT_LIBPQ_TESTS "Build the opt-in independent libpq result controls" OFF)

  add_executable(weave_postgres_configuration_info tests/configuration_info.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_configuration_info PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_configuration_info PRIVATE weave::postgres OpenSSL::Crypto)
  add_executable(weave_postgres_configuration_session tests/configuration_session.cpp)
  target_link_libraries(weave_postgres_configuration_session PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_configuration_session PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_configuration_session PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  foreach(target weave_postgres_configuration_info weave_postgres_configuration_session)
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_configuration_info COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/configuration_info.py"
    --values "$<TARGET_FILE:weave_postgres_configuration_info>"
    --session "$<TARGET_FILE:weave_postgres_configuration_session>" ${result_runtime})
  set_tests_properties(weave_postgres_configuration_info PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_configuration_info PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_option_schema tests/option_schema.cpp)
  target_link_libraries(weave_postgres_option_schema PRIVATE weave::postgres OpenSSL::Crypto)
  weave_no_exceptions(weave_postgres_option_schema)
  weave_postgres_runtime(weave_postgres_option_schema)
  add_test(NAME weave_postgres_option_schema COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/option_schema.py"
    --executable "$<TARGET_FILE:weave_postgres_option_schema>")
  set_tests_properties(weave_postgres_option_schema PROPERTIES TIMEOUT 90)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_option_schema PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_connection_report tests/connection_report.cpp)
  target_link_libraries(weave_postgres_connection_report PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_connection_report PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_connection_report PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  set(report_gss)
  if(WIN32 OR WEAVE_POSTGRES_GSSAPI)
    set(report_gss --gss)
  endif()
  weave_no_exceptions(weave_postgres_connection_report)
  weave_postgres_runtime(weave_postgres_connection_report)
  add_test(NAME weave_postgres_connection_report COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/connection_report.py"
    --executable "$<TARGET_FILE:weave_postgres_connection_report>" ${result_runtime} ${report_gss})
  set_tests_properties(weave_postgres_connection_report PROPERTIES TIMEOUT 240 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_connection_report PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_availability tests/availability.cpp)
  target_include_directories(weave_postgres_availability PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_availability PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_availability PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_availability PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_availability)
  weave_postgres_runtime(weave_postgres_availability)
  add_test(NAME weave_postgres_availability COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/availability.py"
    --executable "$<TARGET_FILE:weave_postgres_availability>" ${result_runtime})
  set_tests_properties(weave_postgres_availability PROPERTIES TIMEOUT 240 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_availability PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_availability_live tests/availability_live.cpp)
  target_include_directories(weave_postgres_availability_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_availability_live PRIVATE weave::postgres OpenSSL::Crypto ${CMAKE_DL_LIBS})
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_availability_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_availability_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_availability_live)
  weave_postgres_runtime(weave_postgres_availability_live)

  if(WEAVE_POSTGRES_RESULT_LIBPQ_TESTS)
    find_package(PostgreSQL 18 REQUIRED)
    if(NOT PostgreSQL_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)")
      message(FATAL_ERROR "Cannot determine the libpq version for independent result controls")
    endif()
    math(EXPR result_libpq_version "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2}")
    add_executable(weave_postgres_result_storage_libpq tests/result_storage_libpq.cpp)
    target_compile_features(weave_postgres_result_storage_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_result_storage_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_result_storage_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_result_storage_libpq)
    weave_tls_runtime(weave_postgres_result_storage_libpq)
    add_test(NAME weave_postgres_result_storage_libpq COMMAND weave_postgres_result_storage_libpq)
    set_tests_properties(weave_postgres_result_storage_libpq PROPERTIES TIMEOUT 30)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_result_storage_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_transaction_libpq tests/transaction_libpq.cpp)
    target_compile_features(weave_postgres_transaction_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_transaction_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_transaction_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_transaction_libpq)
    weave_tls_runtime(weave_postgres_transaction_libpq)
    add_test(NAME weave_postgres_transaction_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/transaction.py"
      --native "$<TARGET_FILE:weave_postgres_transaction_libpq>" --native-only)
    set_tests_properties(weave_postgres_transaction_libpq PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_pipeline_status_libpq tests/pipeline_status_libpq.cpp)
    target_compile_features(weave_postgres_pipeline_status_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_pipeline_status_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_pipeline_status_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_pipeline_status_libpq)
    weave_tls_runtime(weave_postgres_pipeline_status_libpq)
    add_test(NAME weave_postgres_pipeline_status_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_status.py"
      --native "$<TARGET_FILE:weave_postgres_pipeline_status_libpq>" --native-only)
    set_tests_properties(weave_postgres_pipeline_status_libpq PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_result_copy_libpq tests/result_copy_libpq.cpp)
    target_compile_features(weave_postgres_result_copy_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_result_copy_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_result_copy_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_result_copy_libpq)
    weave_tls_runtime(weave_postgres_result_copy_libpq)
    add_test(NAME weave_postgres_result_copy_libpq COMMAND weave_postgres_result_copy_libpq)
    set_tests_properties(weave_postgres_result_copy_libpq PROPERTIES TIMEOUT 30 LABELS integration)

    add_executable(weave_postgres_result_attach_libpq tests/result_attach_libpq.cpp)
    target_compile_features(weave_postgres_result_attach_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_result_attach_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_result_attach_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_result_attach_libpq)
    weave_tls_runtime(weave_postgres_result_attach_libpq)
    add_test(NAME weave_postgres_result_attach_libpq COMMAND weave_postgres_result_attach_libpq)
    set_tests_properties(weave_postgres_result_attach_libpq PROPERTIES TIMEOUT 30 LABELS integration)

    add_executable(weave_postgres_flush_request_libpq tests/flush_request_libpq.cpp)
    target_compile_features(weave_postgres_flush_request_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_flush_request_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_flush_request_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_flush_request_libpq)
    weave_tls_runtime(weave_postgres_flush_request_libpq)
    add_test(NAME weave_postgres_flush_request_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/flush_request.py"
      --native "$<TARGET_FILE:weave_postgres_flush_request_libpq>")
    set_tests_properties(weave_postgres_flush_request_libpq PROPERTIES TIMEOUT 60 LABELS integration)

    add_executable(weave_postgres_exchange_chunks_libpq tests/exchange_chunks_libpq.cpp)
    target_compile_features(weave_postgres_exchange_chunks_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_exchange_chunks_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_exchange_chunks_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_exchange_chunks_libpq)
    weave_tls_runtime(weave_postgres_exchange_chunks_libpq)
    add_test(NAME weave_postgres_exchange_chunks_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/exchange_chunks.py"
      --native "$<TARGET_FILE:weave_postgres_exchange_chunks_libpq>")
    set_tests_properties(weave_postgres_exchange_chunks_libpq PROPERTIES TIMEOUT 60 LABELS integration)

    add_executable(weave_postgres_configuration_info_libpq tests/configuration_info_libpq.cpp)
    target_compile_features(weave_postgres_configuration_info_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_configuration_info_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_configuration_info_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_configuration_info_libpq)
    weave_tls_runtime(weave_postgres_configuration_info_libpq)
    add_test(NAME weave_postgres_configuration_info_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/configuration_info.py"
      --native "$<TARGET_FILE:weave_postgres_configuration_info_libpq>")
    set_tests_properties(weave_postgres_configuration_info_libpq PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_option_schema_libpq tests/option_schema_libpq.cpp)
    target_compile_features(weave_postgres_option_schema_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_option_schema_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_option_schema_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_option_schema_libpq)
    weave_tls_runtime(weave_postgres_option_schema_libpq)
    add_test(NAME weave_postgres_option_schema_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/option_schema.py"
      --executable "$<TARGET_FILE:weave_postgres_option_schema>"
      --native "$<TARGET_FILE:weave_postgres_option_schema_libpq>")
    set_tests_properties(weave_postgres_option_schema_libpq PROPERTIES TIMEOUT 90)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_option_schema_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()

    add_executable(weave_postgres_authentication_metadata_libpq tests/authentication_metadata_libpq.cpp)
    target_compile_features(weave_postgres_authentication_metadata_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_authentication_metadata_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_authentication_metadata_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_authentication_metadata_libpq)
    weave_tls_runtime(weave_postgres_authentication_metadata_libpq)
    add_test(NAME weave_postgres_authentication_metadata_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/authentication_metadata.py"
      --executable "$<TARGET_FILE:weave_postgres_authentication_metadata_libpq>" --native)
    set_tests_properties(weave_postgres_authentication_metadata_libpq PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_session_metadata_libpq tests/session_metadata_libpq.cpp)
    target_compile_features(weave_postgres_session_metadata_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_session_metadata_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_session_metadata_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_session_metadata_libpq)
    weave_tls_runtime(weave_postgres_session_metadata_libpq)
    add_test(NAME weave_postgres_session_metadata_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_metadata.py"
      --executable "$<TARGET_FILE:weave_postgres_session_metadata_libpq>" --native)
    set_tests_properties(weave_postgres_session_metadata_libpq PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_connection_report_libpq tests/connection_report_libpq.cpp)
    target_compile_features(weave_postgres_connection_report_libpq PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_connection_report_libpq PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_connection_report_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_connection_report_libpq)
    weave_tls_runtime(weave_postgres_connection_report_libpq)
    add_test(NAME weave_postgres_connection_report_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/connection_report.py"
      --executable "$<TARGET_FILE:weave_postgres_connection_report_libpq>" --native)
    set_tests_properties(weave_postgres_connection_report_libpq PROPERTIES TIMEOUT 90 LABELS integration)
    # The independent native functional control does not inherit Weave's sanitizer instrumentation.
    add_executable(weave_postgres_availability_live_libpq tests/availability_live_libpq.cpp)
    target_compile_features(weave_postgres_availability_live_libpq PRIVATE cxx_std_23)
    target_include_directories(weave_postgres_availability_live_libpq PRIVATE "${PROJECT_SOURCE_DIR}/modules/core/include")
    target_link_libraries(weave_postgres_availability_live_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::Crypto)
    target_compile_definitions(weave_postgres_availability_live_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_availability_live_libpq)
    weave_tls_runtime(weave_postgres_availability_live_libpq)

    add_executable(weave_postgres_availability_libpq tests/availability.cpp)
    target_include_directories(weave_postgres_availability_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_availability_libpq PRIVATE weave::postgres PostgreSQL::PostgreSQL OpenSSL::Crypto)
    target_compile_definitions(weave_postgres_availability_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_availability_libpq)
    weave_postgres_runtime(weave_postgres_availability_libpq)
    add_test(NAME weave_postgres_availability_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/availability.py"
      --executable "$<TARGET_FILE:weave_postgres_availability_libpq>"
      --libpq-version "${result_libpq_version}" --native)
    set_tests_properties(weave_postgres_availability_libpq PROPERTIES TIMEOUT 90 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_availability_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_target_diagnostic_libpq tests/target_diagnostic.cpp)
    target_link_libraries(weave_postgres_target_diagnostic_libpq PRIVATE weave::postgres PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_target_diagnostic_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_target_diagnostic_libpq)
    weave_postgres_runtime(weave_postgres_target_diagnostic_libpq)
    add_test(NAME weave_postgres_target_diagnostic_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_diagnostic.py"
      --executable "$<TARGET_FILE:weave_postgres_target_diagnostic_libpq>" --native)
    set_tests_properties(weave_postgres_target_diagnostic_libpq PROPERTIES TIMEOUT 60 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_target_diagnostic_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_status_libpq tests/status_libpq.cpp)
    target_link_libraries(weave_postgres_status_libpq PRIVATE weave::postgres PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_status_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_status_libpq)
    weave_postgres_runtime(weave_postgres_status_libpq)
    add_test(NAME weave_postgres_status_libpq COMMAND weave_postgres_status_libpq)
    set_tests_properties(weave_postgres_status_libpq PROPERTIES TIMEOUT 60)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_status_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_diagnostic_libpq tests/diagnostic_libpq.cpp)
    target_include_directories(weave_postgres_diagnostic_libpq PRIVATE src)
    target_link_libraries(weave_postgres_diagnostic_libpq PRIVATE weave::postgres PostgreSQL::PostgreSQL)
    weave_no_exceptions(weave_postgres_diagnostic_libpq)
    weave_postgres_runtime(weave_postgres_diagnostic_libpq)
    add_test(NAME weave_postgres_diagnostic_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostic_libpq.py"
      --executable "$<TARGET_FILE:weave_postgres_diagnostic_libpq>" --libpq-version "${result_libpq_version}")
    set_tests_properties(weave_postgres_diagnostic_libpq PROPERTIES TIMEOUT 240 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_diagnostic_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_column_lookup_libpq tests/column_lookup_libpq.cpp)
    target_link_libraries(weave_postgres_column_lookup_libpq PRIVATE weave::postgres PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_column_lookup_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${result_libpq_version})
    weave_no_exceptions(weave_postgres_column_lookup_libpq)
    weave_postgres_runtime(weave_postgres_column_lookup_libpq)
    add_test(NAME weave_postgres_column_lookup_libpq COMMAND weave_postgres_column_lookup_libpq)
    set_tests_properties(weave_postgres_column_lookup_libpq PROPERTIES TIMEOUT 60)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_column_lookup_libpq PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    add_executable(weave_postgres_results_libpq tests/results_libpq.cpp)
    target_compile_features(weave_postgres_results_libpq PRIVATE cxx_std_23)
    target_include_directories(weave_postgres_results_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_results_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_results_libpq)
    weave_postgres_runtime(weave_postgres_results_libpq)
    if(WIN32)
      get_filename_component(pq_library_directory "${PostgreSQL_LIBRARY_RELEASE}" DIRECTORY)
      get_filename_component(pq_prefix "${pq_library_directory}" DIRECTORY)
      file(GLOB pq_runtime_dlls "${pq_prefix}/bin/*.dll")
      foreach(dll IN LISTS pq_runtime_dlls)
        add_custom_command(TARGET weave_postgres_result_storage_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_result_storage_libpq>")
        add_custom_command(TARGET weave_postgres_result_attach_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_result_attach_libpq>")
        add_custom_command(TARGET weave_postgres_flush_request_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_flush_request_libpq>")
        add_custom_command(TARGET weave_postgres_exchange_chunks_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_exchange_chunks_libpq>")
        add_custom_command(TARGET weave_postgres_result_copy_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_result_copy_libpq>")
        add_custom_command(TARGET weave_postgres_pipeline_status_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_pipeline_status_libpq>")
        add_custom_command(TARGET weave_postgres_transaction_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_transaction_libpq>")
        add_custom_command(TARGET weave_postgres_configuration_info_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_configuration_info_libpq>")
        add_custom_command(TARGET weave_postgres_availability_live_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_availability_live_libpq>")
        add_custom_command(TARGET weave_postgres_availability_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_availability_libpq>")
        add_custom_command(TARGET weave_postgres_target_diagnostic_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_target_diagnostic_libpq>")
        add_custom_command(TARGET weave_postgres_status_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_status_libpq>")
        add_custom_command(TARGET weave_postgres_diagnostic_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_diagnostic_libpq>")
        add_custom_command(TARGET weave_postgres_column_lookup_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_column_lookup_libpq>")
        add_custom_command(TARGET weave_postgres_results_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_results_libpq>")
      endforeach()
    endif()
  endif()

  add_executable(weave_postgres_portal_description tests/portal_description.cpp)
  target_link_libraries(weave_postgres_portal_description PRIVATE weave::postgres)
  add_executable(weave_postgres_portal_description_live tests/portal_description_live.cpp)
  target_include_directories(weave_postgres_portal_description_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_portal_description_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(description_runtime)
  if(TARGET weave_runtime)
    foreach(target weave_postgres_portal_description weave_postgres_portal_description_live)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endforeach()
    set(description_runtime --runtime)
  endif()
  foreach(target weave_postgres_portal_description weave_postgres_portal_description_live)
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_portal_description COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/portal_description.py"
    --executable "$<TARGET_FILE:weave_postgres_portal_description>" ${description_runtime})
  set_tests_properties(weave_postgres_portal_description PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_portal_description PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  option(WEAVE_POSTGRES_DESCRIPTION_LIBPQ_TESTS "Build the opt-in independent libpq portal control" OFF)
  if(WEAVE_POSTGRES_DESCRIPTION_LIBPQ_TESTS)
    find_package(PostgreSQL 18 REQUIRED)
    add_executable(weave_postgres_portal_description_libpq tests/portal_description_libpq.cpp)
    target_compile_features(weave_postgres_portal_description_libpq PRIVATE cxx_std_23)
    target_include_directories(weave_postgres_portal_description_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_portal_description_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_portal_description_libpq)
    weave_postgres_runtime(weave_postgres_portal_description_libpq)
    if(WIN32)
      get_filename_component(pq_library_directory "${PostgreSQL_LIBRARY_RELEASE}" DIRECTORY)
      get_filename_component(pq_prefix "${pq_library_directory}" DIRECTORY)
      file(GLOB pq_runtime_dlls "${pq_prefix}/bin/*.dll")
      foreach(dll IN LISTS pq_runtime_dlls)
        add_custom_command(TARGET weave_postgres_portal_description_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_portal_description_libpq>")
      endforeach()
    endif()
  endif()

  add_executable(weave_postgres_metadata_live tests/metadata_live.cpp)
  target_include_directories(weave_postgres_metadata_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_metadata_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_metadata_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_metadata_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_metadata_live)
  weave_postgres_runtime(weave_postgres_metadata_live)

  add_executable(weave_postgres_password_hashes tests/password_hashes.cpp)
  target_link_libraries(weave_postgres_password_hashes PRIVATE weave::postgres)
  weave_no_exceptions(weave_postgres_password_hashes)
  weave_postgres_runtime(weave_postgres_password_hashes)
  add_test(NAME weave_postgres_password_hashes COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/password_hashes.py"
    --executable "$<TARGET_FILE:weave_postgres_password_hashes>")
  set_tests_properties(weave_postgres_password_hashes PROPERTIES TIMEOUT 30)

  add_executable(weave_postgres_password_session tests/password_session.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_password_session PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_password_session PRIVATE weave::postgres)
  weave_no_exceptions(weave_postgres_password_session)
  weave_postgres_runtime(weave_postgres_password_session)
  add_test(NAME weave_postgres_password_session COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/password_sessions.py"
    --executable "$<TARGET_FILE:weave_postgres_password_session>")
  set_tests_properties(weave_postgres_password_session PROPERTIES TIMEOUT 90 LABELS integration)

  add_executable(weave_postgres_password_live tests/password_live.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_password_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_compile_definitions(weave_postgres_password_live PRIVATE WEAVE_CREDENTIAL_ALLOCATION_SLOTS=32768)
  target_link_libraries(weave_postgres_password_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_password_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_password_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_password_live)
  weave_postgres_runtime(weave_postgres_password_live)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_password_hashes weave_postgres_password_session PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_tls_negotiation tests/tls_negotiation.cpp)
  target_include_directories(weave_postgres_tls_negotiation PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_tls_negotiation PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(tls_negotiation_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_tls_negotiation PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_tls_negotiation PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(tls_negotiation_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_tls_negotiation)
  weave_postgres_runtime(weave_postgres_tls_negotiation)
  add_test(NAME weave_postgres_tls_negotiation COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_negotiation.py"
    --executable "$<TARGET_FILE:weave_postgres_tls_negotiation>" ${tls_negotiation_runtime})
  set_tests_properties(weave_postgres_tls_negotiation PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_tls_negotiation PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_tls_negotiation_live tests/tls_negotiation_live.cpp)
  target_include_directories(weave_postgres_tls_negotiation_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_tls_negotiation_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_tls_negotiation_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_tls_negotiation_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_tls_negotiation_live)
  weave_postgres_runtime(weave_postgres_tls_negotiation_live)

  add_executable(weave_postgres_sni tests/sni.cpp)
  target_include_directories(weave_postgres_sni PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_sni PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(sni_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_sni PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_sni PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(sni_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_sni)
  weave_postgres_runtime(weave_postgres_sni)
  add_test(NAME weave_postgres_sni COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/sni.py" --executable "$<TARGET_FILE:weave_postgres_sni>" ${sni_runtime})
  add_test(NAME weave_postgres_sni_configuration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/sni_configuration.py" --executable "$<TARGET_FILE:weave_postgres_sni>")
  set_tests_properties(weave_postgres_sni weave_postgres_sni_configuration PROPERTIES TIMEOUT 180 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_sni weave_postgres_sni_configuration PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_client_certificates tests/client_certificates.cpp)
  target_include_directories(weave_postgres_client_certificates PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_client_certificates PRIVATE weave::postgres OpenSSL::Crypto)
  set(client_certificates_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_client_certificates PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_client_certificates PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(client_certificates_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_client_certificates)
  weave_postgres_runtime(weave_postgres_client_certificates)
  add_test(NAME weave_postgres_client_certificates COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates.py"
    --executable "$<TARGET_FILE:weave_postgres_client_certificates>" ${client_certificates_runtime})
  add_test(NAME weave_postgres_client_certificates_plaintext COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates.py"
    --executable "$<TARGET_FILE:weave_postgres_client_certificates>" ${client_certificates_runtime} --plaintext-only)
  add_test(NAME weave_postgres_client_certificates_oauth COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates.py"
    --executable "$<TARGET_FILE:weave_postgres_client_certificates>" ${client_certificates_runtime} --oauth-only)
  set_tests_properties(weave_postgres_client_certificates weave_postgres_client_certificates_plaintext
    weave_postgres_client_certificates_oauth PROPERTIES TIMEOUT 240 LABELS integration)

  add_executable(weave_postgres_client_certificates_configuration tests/client_certificates_configuration.cpp)
  target_link_libraries(weave_postgres_client_certificates_configuration PRIVATE weave::postgres)
  weave_no_exceptions(weave_postgres_client_certificates_configuration)
  weave_postgres_runtime(weave_postgres_client_certificates_configuration)
  add_test(NAME weave_postgres_client_certificates_parser COMMAND weave_postgres_client_certificates_configuration)
  add_test(NAME weave_postgres_client_certificates_configuration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates_configuration.py"
    --executable "$<TARGET_FILE:weave_postgres_client_certificates_configuration>")
  set_tests_properties(weave_postgres_client_certificates_parser weave_postgres_client_certificates_configuration
    PROPERTIES TIMEOUT 90)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_client_certificates weave_postgres_client_certificates_plaintext
      weave_postgres_client_certificates_oauth weave_postgres_client_certificates_parser
      weave_postgres_client_certificates_configuration PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  option(WEAVE_POSTGRES_TLS_LIBPQ_TESTS "Build opt-in independent libpq TLS negotiation, SNI and certificate controls" OFF)
  if(WEAVE_POSTGRES_TLS_LIBPQ_TESTS)
    find_package(PostgreSQL 18 REQUIRED)
    if(NOT PostgreSQL_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)")
      message(FATAL_ERROR "Cannot determine libpq version for TLS negotiation controls")
    endif()
    math(EXPR tls_libpq_version "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2}")
    add_executable(weave_postgres_tls_modes_libpq tests/tls_modes_libpq.cpp)
    target_compile_features(weave_postgres_tls_modes_libpq PRIVATE cxx_std_23)
    target_compile_definitions(weave_postgres_tls_modes_libpq PRIVATE WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${tls_libpq_version})
    target_include_directories(weave_postgres_tls_modes_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_tls_modes_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_tls_modes_libpq)
    weave_tls_runtime(weave_postgres_tls_modes_libpq)
    add_test(NAME weave_postgres_tls_modes_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_modes.py" --executable "$<TARGET_FILE:weave_postgres_tls_modes_libpq>" --native)
    set_tests_properties(weave_postgres_tls_modes_libpq PROPERTIES TIMEOUT 120 LABELS integration)
    add_executable(weave_postgres_client_certificates_libpq tests/client_certificates_libpq.cpp)
    target_compile_features(weave_postgres_client_certificates_libpq PRIVATE cxx_std_23)
    target_compile_definitions(weave_postgres_client_certificates_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${tls_libpq_version})
    target_include_directories(weave_postgres_client_certificates_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_client_certificates_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_client_certificates_libpq)
    weave_tls_runtime(weave_postgres_client_certificates_libpq)
    add_test(NAME weave_postgres_client_certificates_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates.py"
      --executable "$<TARGET_FILE:weave_postgres_client_certificates_libpq>" --native)
    add_test(NAME weave_postgres_client_certificates_plaintext_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates.py"
      --executable "$<TARGET_FILE:weave_postgres_client_certificates_libpq>" --native --plaintext-only)
    set_tests_properties(weave_postgres_client_certificates_libpq weave_postgres_client_certificates_plaintext_libpq
      PROPERTIES TIMEOUT 240 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_client_certificates_libpq weave_postgres_client_certificates_plaintext_libpq
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()

    add_executable(weave_postgres_sni_libpq tests/sni_libpq.cpp)
    target_compile_features(weave_postgres_sni_libpq PRIVATE cxx_std_23)
    target_compile_definitions(weave_postgres_sni_libpq PRIVATE WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${tls_libpq_version})
    target_include_directories(weave_postgres_sni_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_sni_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_sni_libpq)
    weave_tls_runtime(weave_postgres_sni_libpq)
    add_test(NAME weave_postgres_sni_libpq COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/sni.py" --executable "$<TARGET_FILE:weave_postgres_sni_libpq>" --native)
    set_tests_properties(weave_postgres_sni_libpq PROPERTIES TIMEOUT 180 LABELS integration)
    add_executable(weave_postgres_tls_negotiation_libpq tests/tls_negotiation_libpq.cpp)
    target_compile_features(weave_postgres_tls_negotiation_libpq PRIVATE cxx_std_23)
    target_compile_definitions(weave_postgres_tls_negotiation_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${tls_libpq_version})
    target_include_directories(weave_postgres_tls_negotiation_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_tls_negotiation_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_tls_negotiation_libpq)
    weave_tls_runtime(weave_postgres_tls_negotiation_libpq)
    if(WIN32)
      set_target_properties(weave_postgres_tls_negotiation_libpq PROPERTIES
        VS_GLOBAL_VcpkgEnabled false VS_GLOBAL_VcpkgApplocalDeps false)
      set_target_properties(weave_postgres_sni_libpq PROPERTIES
        VS_GLOBAL_VcpkgEnabled false VS_GLOBAL_VcpkgApplocalDeps false)
      set_target_properties(weave_postgres_client_certificates_libpq PROPERTIES
        VS_GLOBAL_VcpkgEnabled false VS_GLOBAL_VcpkgApplocalDeps false)
      set_target_properties(weave_postgres_tls_modes_libpq PROPERTIES
        VS_GLOBAL_VcpkgEnabled false VS_GLOBAL_VcpkgApplocalDeps false)
      get_filename_component(tls_libpq_prefix "${PostgreSQL_INCLUDE_DIR}" DIRECTORY)
      file(GLOB tls_libpq_dlls "${tls_libpq_prefix}/bin/*.dll")
      foreach(dll IN LISTS tls_libpq_dlls)
        add_custom_command(TARGET weave_postgres_tls_modes_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_tls_modes_libpq>")
        add_custom_command(TARGET weave_postgres_client_certificates_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_client_certificates_libpq>")
        add_custom_command(TARGET weave_postgres_sni_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_sni_libpq>")
        add_custom_command(TARGET weave_postgres_tls_negotiation_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_tls_negotiation_libpq>")
      endforeach()
    endif()
  endif()

  set(tls_password_runtime)
  foreach(suite tls_password tls_password_live)
    add_executable(weave_postgres_${suite} tests/${suite}.cpp)
    target_include_directories(weave_postgres_${suite} PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_${suite} PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET weave_runtime)
      target_link_libraries(weave_postgres_${suite} PRIVATE weave::runtime)
      target_compile_definitions(weave_postgres_${suite} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      set(tls_password_runtime --runtime)
    endif()
    weave_no_exceptions(weave_postgres_${suite})
    weave_postgres_runtime(weave_postgres_${suite})
  endforeach()
  add_test(NAME weave_postgres_tls_password COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_password.py"
    --executable "$<TARGET_FILE:weave_postgres_tls_password>" ${tls_password_runtime})
  set_tests_properties(weave_postgres_tls_password PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_tls_password PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  option(WEAVE_POSTGRES_PASSWORD_LIBPQ_TESTS "Build the opt-in independent libpq password controls" OFF)
  if(WEAVE_POSTGRES_PASSWORD_LIBPQ_TESTS)
    find_package(PostgreSQL 18 REQUIRED)
    add_executable(weave_postgres_password_libpq tests/password_libpq.cpp)
    target_compile_features(weave_postgres_password_libpq PRIVATE cxx_std_23)
    target_include_directories(weave_postgres_password_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_password_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_password_libpq)
    if(NOT PostgreSQL_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)")
      message(FATAL_ERROR "Cannot determine libpq version for the private-key hook control")
    endif()
    math(EXPR password_libpq_version "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2}")
    add_executable(weave_postgres_tls_password_libpq tests/tls_password_libpq.cpp)
    target_compile_features(weave_postgres_tls_password_libpq PRIVATE cxx_std_23)
    target_compile_definitions(weave_postgres_tls_password_libpq PRIVATE
      WEAVE_POSTGRES_TEST_LIBPQ_VERSION=${password_libpq_version})
    target_include_directories(weave_postgres_tls_password_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_tls_password_libpq PRIVATE PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_tls_password_libpq)
    weave_tls_runtime(weave_postgres_tls_password_libpq)
    if(WIN32)
      get_filename_component(password_libpq_prefix "${PostgreSQL_INCLUDE_DIR}" DIRECTORY)
      file(GLOB password_libpq_dlls "${password_libpq_prefix}/bin/*.dll")
      foreach(dll IN LISTS password_libpq_dlls)
        add_custom_command(TARGET weave_postgres_tls_password_libpq POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:weave_postgres_tls_password_libpq>")
      endforeach()
    endif()
  endif()

  # Compile the exact loader with an owned directory, without touching system configuration.
  string(ASCII 195 169 service_fixture_suffix)
  set(service_fixture "${CMAKE_CURRENT_BINARY_DIR}/service-fixture-${service_fixture_suffix}")
  weave_postgres_configuration_paths("${service_fixture}" service-test/configuration_paths.hpp)
  add_executable(weave_postgres_system_service tests/system_service.cpp src/configuration.cpp)
  target_include_directories(weave_postgres_system_service PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/service-test")
  target_link_libraries(weave_postgres_system_service PRIVATE weave::postgres OpenSSL::Crypto)
  weave_no_exceptions(weave_postgres_system_service)
  weave_postgres_runtime(weave_postgres_system_service)
  add_test(NAME weave_postgres_system_service COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/system_service.py"
    --executable "$<TARGET_FILE:weave_postgres_system_service>" --directory "${service_fixture}")
  set_tests_properties(weave_postgres_system_service PROPERTIES TIMEOUT 30)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_system_service PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  weave_add_test(weave_postgres_ldap tests/ldap.cpp)
  target_include_directories(weave_postgres_ldap PRIVATE src)
  target_link_libraries(weave_postgres_ldap PRIVATE weave::postgres)
  weave_postgres_runtime(weave_postgres_ldap)
  if(WEAVE_POSTGRES_LDAP)
    target_compile_definitions(weave_postgres_ldap PRIVATE WEAVE_POSTGRES_TEST_LDAP=1)
    add_executable(weave_postgres_ldap_live tests/ldap_live.cpp)
    target_link_libraries(weave_postgres_ldap_live PRIVATE weave::postgres)
    weave_no_exceptions(weave_postgres_ldap_live)
    weave_postgres_runtime(weave_postgres_ldap_live)
  endif()
  option(WEAVE_POSTGRES_LDAP_SERVER_TESTS "Run opt-in owned OpenLDAP qualification on Linux" OFF)
  if(WEAVE_POSTGRES_LDAP_SERVER_TESTS)
    if(NOT WEAVE_POSTGRES_LDAP OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
      message(FATAL_ERROR "WEAVE_POSTGRES_LDAP_SERVER_TESTS requires Linux and WEAVE_POSTGRES_LDAP=ON")
    endif()
    set(WEAVE_POSTGRES_LDAP_TEST_TOOLS "" CACHE PATH "Extracted OpenLDAP tools/schema/modules/runtime root")
    foreach(program usr/sbin/slapd usr/sbin/slapadd etc/ldap/schema/core.schema)
      if(NOT EXISTS "${WEAVE_POSTGRES_LDAP_TEST_TOOLS}/${program}")
        message(FATAL_ERROR "Owned LDAP qualification requires ${program} in WEAVE_POSTGRES_LDAP_TEST_TOOLS")
      endif()
    endforeach()
    add_test(NAME weave_postgres_ldap_server COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ldap.py"
      --executable "$<TARGET_FILE:weave_postgres_ldap_live>" --tools "${WEAVE_POSTGRES_LDAP_TEST_TOOLS}")
    set_tests_properties(weave_postgres_ldap_server PROPERTIES TIMEOUT 120 LABELS integration)
  endif()

  add_executable(weave_postgres_oauth_https tests/oauth_https.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_oauth_https PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_oauth_https PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_oauth_https PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_oauth_https PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_oauth_https)
  weave_postgres_runtime(weave_postgres_oauth_https)
  add_test(NAME weave_postgres_oauth_https COMMAND weave_postgres_oauth_https)
  set_tests_properties(weave_postgres_oauth_https PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_oauth_https PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  weave_add_test(weave_postgres_oauth tests/oauth.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_oauth PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_oauth PRIVATE weave::postgres)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_oauth PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_oauth PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_postgres_runtime(weave_postgres_oauth)

  weave_add_test(weave_postgres_oauth_startup tests/oauth_startup.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_oauth_startup PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_oauth_startup PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_oauth_startup PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_oauth_startup PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_postgres_runtime(weave_postgres_oauth_startup)

  add_executable(weave_postgres_oauth_device tests/oauth_device.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  add_executable(weave_postgres_oauth_device_live tests/oauth_device_live.cpp)
  set(device_runtime)
  foreach(target weave_postgres_oauth_device weave_postgres_oauth_device_live)
    target_include_directories(${target} PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(${target} PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET weave_runtime)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      set(device_runtime --runtime)
    endif()
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_oauth_device COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/oauth_device.py"
    --executable "$<TARGET_FILE:weave_postgres_oauth_device>" ${device_runtime})
  set_tests_properties(weave_postgres_oauth_device PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_oauth_device PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  option(WEAVE_POSTGRES_OAUTH_SERVER_TESTS "Run PostgreSQL 18 OAuth with an owned test-only validator" OFF)
  option(WEAVE_POSTGRES_KEYCLOAK_TESTS "Run opt-in real Keycloak OAuth deployment qualification" OFF)
  if(WEAVE_POSTGRES_KEYCLOAK_TESTS AND NOT WEAVE_POSTGRES_OAUTH_SERVER_TESTS)
    message(FATAL_ERROR "WEAVE_POSTGRES_KEYCLOAK_TESTS requires WEAVE_POSTGRES_OAUTH_SERVER_TESTS")
  endif()
  if(WEAVE_POSTGRES_OAUTH_SERVER_TESTS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
      message(FATAL_ERROR "WEAVE_POSTGRES_OAUTH_SERVER_TESTS requires a Linux PostgreSQL 18 backend")
    endif()
    set(WEAVE_POSTGRES_SERVER_BIN "" CACHE PATH "Directory containing initdb and pg_ctl")
    foreach(program initdb pg_ctl psql)
      if(NOT EXISTS "${WEAVE_POSTGRES_SERVER_BIN}/${program}")
        message(FATAL_ERROR "OAuth qualification requires ${program} in WEAVE_POSTGRES_SERVER_BIN")
      endif()
    endforeach()
    set(WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE "" CACHE PATH "PostgreSQL 18 server headers for the test validator")
    if(NOT EXISTS "${WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE}/postgres.h" OR
       NOT EXISTS "${WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE}/libpq/oauth.h")
      message(FATAL_ERROR "OAuth qualification requires PostgreSQL 18 server headers in WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE")
    endif()
    enable_language(C)
    find_package(PostgreSQL 18 REQUIRED)
    target_link_libraries(weave_postgres_oauth_device PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(weave_postgres_oauth_device PRIVATE WEAVE_POSTGRES_TEST_LIBPQ=1)
    add_library(weave_postgres_oauth_validator MODULE tests/oauth_validator.c)
    target_include_directories(weave_postgres_oauth_validator PRIVATE "${WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE}")
    set_target_properties(weave_postgres_oauth_validator PROPERTIES PREFIX "")

    add_executable(weave_postgres_oauth_live tests/oauth_live.cpp)
    target_include_directories(weave_postgres_oauth_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_oauth_live PRIVATE weave::postgres PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET weave_runtime)
      target_link_libraries(weave_postgres_oauth_live PRIVATE weave::runtime)
      target_compile_definitions(weave_postgres_oauth_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    endif()
    weave_no_exceptions(weave_postgres_oauth_live)
    add_dependencies(weave_postgres_oauth_live weave_postgres_oauth_validator)
    add_test(NAME weave_postgres_oauth_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/oauth_live.py"
      --executable "$<TARGET_FILE:weave_postgres_oauth_live>" --validator "$<TARGET_FILE:weave_postgres_oauth_validator>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_oauth_live PROPERTIES TIMEOUT 210 LABELS integration)
    add_dependencies(weave_postgres_oauth_device_live weave_postgres_oauth_validator)
    add_test(NAME weave_postgres_oauth_device_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/oauth_device.py"
      --executable "$<TARGET_FILE:weave_postgres_oauth_device_live>" ${device_runtime}
      --postgres --validator "$<TARGET_FILE:weave_postgres_oauth_validator>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_oauth_device_live PROPERTIES TIMEOUT 180 LABELS integration)

    if(WEAVE_POSTGRES_KEYCLOAK_TESTS)
      set(WEAVE_POSTGRES_KEYCLOAK_TOOLS "" CACHE PATH "Directory containing the pinned Keycloak and Temurin archives")
      foreach(archive keycloak-26.8.0.tar.gz OpenJDK21U-jre_x64_linux_hotspot_21.0.12.1_1.tar.gz)
        if(NOT EXISTS "${WEAVE_POSTGRES_KEYCLOAK_TOOLS}/${archive}")
          message(FATAL_ERROR "Keycloak qualification requires ${archive} in WEAVE_POSTGRES_KEYCLOAK_TOOLS")
        endif()
      endforeach()
      find_package(CURL REQUIRED)
      find_path(WEAVE_KEYCLOAK_JSON_INCLUDE json-c/json.h REQUIRED)
      find_library(WEAVE_KEYCLOAK_JSON_LIBRARY NAMES json-c REQUIRED)

      add_library(weave_postgres_keycloak_validator MODULE tests/keycloak_validator.c)
      target_include_directories(weave_postgres_keycloak_validator PRIVATE
        "${WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE}" "${WEAVE_KEYCLOAK_JSON_INCLUDE}")
      target_link_libraries(weave_postgres_keycloak_validator PRIVATE
        CURL::libcurl "${WEAVE_KEYCLOAK_JSON_LIBRARY}" OpenSSL::Crypto)
      set_target_properties(weave_postgres_keycloak_validator PROPERTIES PREFIX "")

      add_executable(weave_postgres_oauth_keycloak tests/oauth_keycloak.cpp)
      target_include_directories(weave_postgres_oauth_keycloak PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
      target_link_libraries(weave_postgres_oauth_keycloak PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
      set(keycloak_runtime)
      if(TARGET weave_runtime)
        target_link_libraries(weave_postgres_oauth_keycloak PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_oauth_keycloak PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
        set(keycloak_runtime --runtime)
      endif()
      weave_no_exceptions(weave_postgres_oauth_keycloak)
      add_dependencies(weave_postgres_oauth_keycloak weave_postgres_keycloak_validator)
      add_test(NAME weave_postgres_oauth_keycloak COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/oauth_keycloak.py"
        --executable "$<TARGET_FILE:weave_postgres_oauth_keycloak>"
        --validator "$<TARGET_FILE:weave_postgres_keycloak_validator>" ${keycloak_runtime}
        --tools "${WEAVE_POSTGRES_KEYCLOAK_TOOLS}" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}"
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/keycloak-evidence")
      set_tests_properties(weave_postgres_oauth_keycloak PROPERTIES TIMEOUT 420 LABELS integration RUN_SERIAL TRUE)
    endif()
  endif()

  if(WIN32)
    option(WEAVE_POSTGRES_SSPI_TESTS "Run opt-in native SSPI PostgreSQL startup qualification" OFF)
    if(WEAVE_POSTGRES_SSPI_TESTS)
      add_executable(weave_postgres_sspi tests/gss_sspi.cpp)
      target_include_directories(weave_postgres_sspi PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
      target_link_libraries(weave_postgres_sspi PRIVATE weave::postgres OpenSSL::Crypto secur32 advapi32)
      if(TARGET weave_runtime)
        target_link_libraries(weave_postgres_sspi PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_sspi PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      endif()
      weave_no_exceptions(weave_postgres_sspi)
      weave_postgres_runtime(weave_postgres_sspi)
      add_test(NAME weave_postgres_sspi COMMAND weave_postgres_sspi)
      set_tests_properties(weave_postgres_sspi PROPERTIES TIMEOUT 90 LABELS integration)
      if(WEAVE_ENABLE_ASAN AND MSVC)
        set_tests_properties(weave_postgres_sspi PROPERTIES
          ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      endif()
    endif()
  endif()
  option(WEAVE_POSTGRES_KERBEROS_TESTS "Run native provider tests with an owned disposable Linux KDC" OFF)
  if(WEAVE_POSTGRES_KERBEROS_TESTS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT WEAVE_POSTGRES_GSSAPI)
      message(FATAL_ERROR "WEAVE_POSTGRES_KERBEROS_TESTS requires Linux and WEAVE_POSTGRES_GSSAPI=ON")
    endif()
    find_program(WEAVE_KERBEROS_KDC krb5kdc REQUIRED)
    find_program(WEAVE_KERBEROS_ADMIN kadmin.local REQUIRED)
    find_program(WEAVE_KERBEROS_DATABASE kdb5_util REQUIRED)
    find_program(WEAVE_KERBEROS_KINIT kinit REQUIRED)
    add_executable(weave_postgres_kerberos tests/gss_kerberos.cpp)
    target_include_directories(weave_postgres_kerberos PRIVATE src)
    target_link_libraries(weave_postgres_kerberos PRIVATE weave::postgres PkgConfig::WEAVE_GSSAPI OpenSSL::Crypto)
    weave_no_exceptions(weave_postgres_kerberos)
    add_test(NAME weave_postgres_kerberos COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/gss_kerberos.py"
      --executable "$<TARGET_FILE:weave_postgres_kerberos>" --kdc "${WEAVE_KERBEROS_KDC}"
      --admin "${WEAVE_KERBEROS_ADMIN}" --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}")
    set_tests_properties(weave_postgres_kerberos PROPERTIES TIMEOUT 90 LABELS integration)

    add_executable(weave_postgres_gss_protection tests/gss_protection.cpp)
    target_include_directories(weave_postgres_gss_protection PRIVATE src)
    target_link_libraries(weave_postgres_gss_protection PRIVATE weave::postgres PkgConfig::WEAVE_GSSAPI
      OpenSSL::Crypto ${CMAKE_DL_LIBS})
    set(protection_runtime)
    if(TARGET weave_runtime)
      target_link_libraries(weave_postgres_gss_protection PRIVATE weave::runtime)
      target_compile_definitions(weave_postgres_gss_protection PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      set(protection_runtime --runtime)
    endif()
    weave_no_exceptions(weave_postgres_gss_protection)
    add_library(weave_postgres_gss_audit SHARED tests/gss_protection_audit.cpp)
    target_compile_features(weave_postgres_gss_audit PRIVATE cxx_std_23)
    target_link_libraries(weave_postgres_gss_audit PRIVATE PkgConfig::WEAVE_GSSAPI ${CMAKE_DL_LIBS})
    weave_no_exceptions(weave_postgres_gss_audit)
    set(protection_asan)
    if(WEAVE_ENABLE_ASAN)
      execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libasan.so
        OUTPUT_VARIABLE protection_asan_library OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
      if(NOT EXISTS "${protection_asan_library}")
        message(FATAL_ERROR "GSS protection qualification requires the ASan shared runtime")
      endif()
      set(protection_asan --asan "${protection_asan_library}")
    endif()
    add_test(NAME weave_postgres_gss_protection COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/gss_kerberos.py"
      --executable "$<TARGET_FILE:weave_postgres_gss_protection>" --protection
      --audit "$<TARGET_FILE:weave_postgres_gss_audit>" --kdc "${WEAVE_KERBEROS_KDC}" --admin "${WEAVE_KERBEROS_ADMIN}"
      --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}" ${protection_runtime} ${protection_asan})
    set_tests_properties(weave_postgres_gss_protection PROPERTIES TIMEOUT 90 LABELS integration)
  endif()

  weave_add_test(weave_postgres_credentials tests/credentials.cpp
    "${PROJECT_SOURCE_DIR}/test_support/credential_allocations.cpp")
  target_include_directories(weave_postgres_credentials PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_credentials PRIVATE weave::postgres)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_credentials PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_credentials PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_postgres_runtime(weave_postgres_credentials)

  add_executable(weave_postgres_encoding_session tests/encoding_session.cpp)
  target_link_libraries(weave_postgres_encoding_session PRIVATE weave::postgres)
  set(encoding_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_encoding_session PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_encoding_session PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(encoding_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_encoding_session)
  weave_postgres_runtime(weave_postgres_encoding_session)
  add_test(NAME weave_postgres_encoding_session
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding_session.py"
    --executable "$<TARGET_FILE:weave_postgres_encoding_session>" ${encoding_runtime})
  set_tests_properties(weave_postgres_encoding_session PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_encoding_session PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_notices tests/notices.cpp)
  target_link_libraries(weave_postgres_notices PRIVATE weave::postgres)
  set(notices_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_notices PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_notices PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(notices_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_notices)
  weave_postgres_runtime(weave_postgres_notices)
  add_test(NAME weave_postgres_notices COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/notices.py"
    --executable "$<TARGET_FILE:weave_postgres_notices>" ${notices_runtime})
  set_tests_properties(weave_postgres_notices PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_notices PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_notifications tests/notifications.cpp)
  target_link_libraries(weave_postgres_notifications PRIVATE weave::postgres)
  set(notifications_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_notifications PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_notifications PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(notifications_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_notifications)
  weave_postgres_runtime(weave_postgres_notifications)
  add_test(NAME weave_postgres_notifications COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/notifications.py"
    --executable "$<TARGET_FILE:weave_postgres_notifications>" ${notifications_runtime})
  set_tests_properties(weave_postgres_notifications PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_notifications PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_events tests/events.cpp)
  target_link_libraries(weave_postgres_events PRIVATE weave::postgres)
  set(events_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_events PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_events PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(events_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_events)
  weave_postgres_runtime(weave_postgres_events)
  add_test(NAME weave_postgres_events COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/events.py"
    --executable "$<TARGET_FILE:weave_postgres_events>" ${events_runtime})
  set_tests_properties(weave_postgres_events PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_events PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_result_attach tests/result_attach.cpp)
  add_executable(weave_postgres_result_attach_live tests/result_attach.cpp)
  target_compile_definitions(weave_postgres_result_attach_live PRIVATE WEAVE_POSTGRES_TEST_LIVE=1)
  target_include_directories(weave_postgres_result_attach_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_result_attach_live PRIVATE OpenSSL::Crypto)
  set(result_attach_runtime)
  foreach(target weave_postgres_result_attach weave_postgres_result_attach_live)
    target_link_libraries(${target} PRIVATE weave::postgres)
    if(TARGET weave_runtime)
      target_link_libraries(${target} PRIVATE weave::runtime)
      target_compile_definitions(${target} PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      set(result_attach_runtime --runtime)
    endif()
    weave_no_exceptions(${target})
    weave_postgres_runtime(${target})
  endforeach()
  add_test(NAME weave_postgres_result_attach COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/result_attach.py"
    --executable "$<TARGET_FILE:weave_postgres_result_attach>" ${result_attach_runtime})
  set_tests_properties(weave_postgres_result_attach PROPERTIES TIMEOUT 120 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_result_attach PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_trace tests/trace.cpp)
  target_include_directories(weave_postgres_trace PRIVATE src)
  target_link_libraries(weave_postgres_trace PRIVATE weave::postgres)
  weave_no_exceptions(weave_postgres_trace)
  weave_postgres_runtime(weave_postgres_trace)
  add_test(NAME weave_postgres_trace COMMAND weave_postgres_trace)
  set_tests_properties(weave_postgres_trace PROPERTIES TIMEOUT 30 LABELS integration)

  add_executable(weave_postgres_trace_sessions tests/trace_sessions.cpp)
  target_link_libraries(weave_postgres_trace_sessions PRIVATE weave::postgres)
  set(trace_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_trace_sessions PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_trace_sessions PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(trace_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_trace_sessions)
  weave_postgres_runtime(weave_postgres_trace_sessions)
  add_test(NAME weave_postgres_trace_sessions COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/trace_sessions.py"
    --executable "$<TARGET_FILE:weave_postgres_trace_sessions>" ${trace_runtime})
  set_tests_properties(weave_postgres_trace_sessions PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_trace weave_postgres_trace_sessions PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_exchange tests/exchange.cpp)
  target_link_libraries(weave_postgres_exchange PRIVATE weave::postgres)
  set(exchange_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_exchange PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_exchange PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(exchange_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_exchange)
  weave_postgres_runtime(weave_postgres_exchange)
  add_test(NAME weave_postgres_exchange COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/exchange.py"
    --executable "$<TARGET_FILE:weave_postgres_exchange>" ${exchange_runtime})
  set_tests_properties(weave_postgres_exchange PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_exchange PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_test(NAME weave_postgres_benchmark_tooling
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_tooling.py")
  set_tests_properties(weave_postgres_benchmark_tooling PROPERTIES TIMEOUT 30 LABELS tooling)

  add_executable(weave_postgres_cancellation tests/cancellation.cpp)
  target_include_directories(weave_postgres_cancellation PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_cancellation PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  set(cancellation_runtime)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_cancellation PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_cancellation PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
    set(cancellation_runtime --runtime)
  endif()
  weave_no_exceptions(weave_postgres_cancellation)
  weave_postgres_runtime(weave_postgres_cancellation)
  add_test(NAME weave_postgres_cancellation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cancellation.py"
    --executable "$<TARGET_FILE:weave_postgres_cancellation>" ${cancellation_runtime})
  set_tests_properties(weave_postgres_cancellation PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_cancellation PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_authentication tests/authentication.cpp)
  target_include_directories(weave_postgres_authentication PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_authentication PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  weave_no_exceptions(weave_postgres_authentication)
  weave_postgres_runtime(weave_postgres_authentication)
  add_test(NAME weave_postgres_authentication COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/authentication.py"
    --executable "$<TARGET_FILE:weave_postgres_authentication>")
  set_tests_properties(weave_postgres_authentication PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_authentication PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  weave_add_test(weave_postgres_tests tests/wire.cpp tests/network.cpp tests/options.cpp tests/pipeline.cpp
    tests/pipeline_streaming.cpp tests/copy_both.cpp tests/local.cpp tests/encoding.cpp tests/column_lookup.cpp
    tests/diagnostic.cpp tests/status.cpp tests/gss.cpp tests/result_format.cpp)
  target_include_directories(weave_postgres_tests PRIVATE src)
  target_link_libraries(weave_postgres_tests PRIVATE weave::postgres weave::local)
  if(WEAVE_POSTGRES_GSSAPI)
    target_compile_definitions(weave_postgres_tests PRIVATE WEAVE_POSTGRES_TEST_GSSAPI=1)
  endif()
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_tests PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_tests PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_postgres_runtime(weave_postgres_tests)

  add_executable(weave_postgres_configuration_tests tests/configuration.cpp)
  if(NOT WIN32)
    weave_add_test(weave_postgres_peer_configuration tests/peer_configuration.cpp)
    target_link_libraries(weave_postgres_peer_configuration PRIVATE weave::postgres)
    target_link_options(weave_postgres_peer_configuration PRIVATE -Wl,--wrap=getpwnam_r -Wl,--wrap=getpwuid_r)
  endif()
  if(WEAVE_POSTGRES_GSSAPI)
    target_compile_definitions(weave_postgres_configuration_tests PRIVATE WEAVE_POSTGRES_TEST_GSSAPI=1)
  endif()
  target_link_libraries(weave_postgres_configuration_tests PRIVATE weave::postgres doctest::doctest)
  target_compile_definitions(weave_postgres_configuration_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
  weave_no_exceptions(weave_postgres_configuration_tests)
  weave_postgres_runtime(weave_postgres_configuration_tests)
  add_test(NAME weave_postgres_configuration COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/configuration.py"
    --executable "$<TARGET_FILE:weave_postgres_configuration_tests>")
  set_tests_properties(weave_postgres_configuration PROPERTIES TIMEOUT 30)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_configuration PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_interop tests/interop.cpp)
  target_include_directories(weave_postgres_interop PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_interop PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  weave_no_exceptions(weave_postgres_interop)
  weave_postgres_runtime(weave_postgres_interop)
  add_test(NAME weave_postgres_interop COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop.py"
    --executable "$<TARGET_FILE:weave_postgres_interop>")
  set_tests_properties(weave_postgres_interop PROPERTIES TIMEOUT 90 LABELS integration)
  if(WEAVE_ENABLE_ASAN AND MSVC)
    set_tests_properties(weave_postgres_interop PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
  endif()

  add_executable(weave_postgres_live tests/live.cpp tests/pipeline_live.cpp tests/replication_live.cpp tests/authentication_live.cpp)
  target_include_directories(weave_postgres_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_live PRIVATE WEAVE_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_live)
  weave_postgres_runtime(weave_postgres_live)

  add_executable(weave_postgres_notifications_live tests/notifications_live.cpp)
  target_include_directories(weave_postgres_notifications_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_notifications_live PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_notifications_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_notifications_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_notifications_live)
  weave_postgres_runtime(weave_postgres_notifications_live)

  add_executable(weave_postgres_events_live tests/events_live.cpp)
  target_include_directories(weave_postgres_events_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_events_live PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_events_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_events_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_events_live)
  weave_postgres_runtime(weave_postgres_events_live)

  add_executable(weave_postgres_trace_live tests/trace_live.cpp)
  target_include_directories(weave_postgres_trace_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_trace_live PRIVATE weave::postgres OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_trace_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_trace_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_trace_live)
  weave_postgres_runtime(weave_postgres_trace_live)

  add_executable(weave_postgres_encoding_live tests/encoding_live.cpp)
  target_include_directories(weave_postgres_encoding_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_encoding_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_encoding_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_encoding_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_encoding_live)
  weave_postgres_runtime(weave_postgres_encoding_live)

  add_executable(weave_postgres_exchange_live tests/exchange_live.cpp)
  target_include_directories(weave_postgres_exchange_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_exchange_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
  if(TARGET weave_runtime)
    target_link_libraries(weave_postgres_exchange_live PRIVATE weave::runtime)
    target_compile_definitions(weave_postgres_exchange_live PRIVATE WEAVE_TEST_RUNTIME=1)
  endif()
  weave_no_exceptions(weave_postgres_exchange_live)
  weave_postgres_runtime(weave_postgres_exchange_live)

  option(WEAVE_POSTGRES_SERVER_TESTS "Run opt-in qualification against a disposable real PostgreSQL server" OFF)
  if(WEAVE_POSTGRES_SERVER_TESTS)
    set(WEAVE_POSTGRES_SERVER_BIN "" CACHE PATH "Directory containing initdb and pg_ctl")
    add_test(NAME weave_postgres_tls_modes_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_tls_modes>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_tls_modes_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_tls_modes_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    foreach(program initdb pg_ctl psql pg_basebackup)
      if(NOT EXISTS "${WEAVE_POSTGRES_SERVER_BIN}/${program}${CMAKE_EXECUTABLE_SUFFIX}")
        message(FATAL_ERROR "WEAVE_POSTGRES_SERVER_TESTS requires ${program} in WEAVE_POSTGRES_SERVER_BIN")
      endif()
    endforeach()
    if(WIN32)
      add_executable(weave_postgres_client_certificates_live tests/client_certificates_live.cpp)
      target_include_directories(weave_postgres_client_certificates_live PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
      target_link_libraries(weave_postgres_client_certificates_live PRIVATE weave::postgres OpenSSL::SSL OpenSSL::Crypto)
      if(TARGET weave_runtime)
        target_link_libraries(weave_postgres_client_certificates_live PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_client_certificates_live PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
      endif()
      weave_no_exceptions(weave_postgres_client_certificates_live)
      weave_postgres_runtime(weave_postgres_client_certificates_live)
      add_test(NAME weave_postgres_client_certificates_live COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates_live.py"
        --executable "$<TARGET_FILE:weave_postgres_client_certificates_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_client_certificates_live PROPERTIES TIMEOUT 240 LABELS integration)
      if(WEAVE_ENABLE_ASAN AND MSVC)
        set_tests_properties(weave_postgres_client_certificates_live PROPERTIES
          ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      endif()
      if(WEAVE_POSTGRES_RESULT_LIBPQ_TESTS)
        add_executable(weave_postgres_client_certificates_live_libpq tests/client_certificates_live_libpq.cpp)
        target_compile_features(weave_postgres_client_certificates_live_libpq PRIVATE cxx_std_23)
        target_include_directories(weave_postgres_client_certificates_live_libpq PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
        target_link_libraries(weave_postgres_client_certificates_live_libpq PRIVATE
          PostgreSQL::PostgreSQL OpenSSL::SSL OpenSSL::Crypto ws2_32)
        weave_no_exceptions(weave_postgres_client_certificates_live_libpq)
        weave_postgres_runtime(weave_postgres_client_certificates_live_libpq)
        set_target_properties(weave_postgres_client_certificates_live_libpq PROPERTIES
          VS_GLOBAL_VcpkgEnabled false VS_GLOBAL_VcpkgApplocalDeps false)
        get_filename_component(certificate_libpq_prefix "${PostgreSQL_INCLUDE_DIR}" DIRECTORY)
        file(GLOB certificate_libpq_dlls "${certificate_libpq_prefix}/bin/*.dll")
        foreach(dll IN LISTS certificate_libpq_dlls)
          add_custom_command(TARGET weave_postgres_client_certificates_live_libpq POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}"
            "$<TARGET_FILE_DIR:weave_postgres_client_certificates_live_libpq>")
        endforeach()
        add_test(NAME weave_postgres_client_certificates_live_libpq COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_certificates_live.py"
          --executable "$<TARGET_FILE:weave_postgres_client_certificates_live_libpq>"
          --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --native)
        set_tests_properties(weave_postgres_client_certificates_live_libpq PROPERTIES TIMEOUT 240 LABELS integration)
      endif()
    endif()
    foreach(suite result_storage_live result_storage_paths)
      add_test(NAME weave_postgres_${suite} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_${suite}>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_${suite} PROPERTIES TIMEOUT 180 LABELS integration)
      if(WEAVE_ENABLE_ASAN AND MSVC)
        set_tests_properties(weave_postgres_${suite} PROPERTIES
          ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      endif()
    endforeach()
    add_test(NAME weave_postgres_results_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_results_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_results_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_results_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    if(WEAVE_POSTGRES_RESULT_LIBPQ_TESTS)
      add_test(NAME weave_postgres_results_libpq COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_results_libpq>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_results_libpq PROPERTIES TIMEOUT 180 LABELS integration)
    endif()
    add_test(NAME weave_postgres_portal_description_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_portal_description_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_portal_description_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_portal_description_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    if(WEAVE_POSTGRES_DESCRIPTION_LIBPQ_TESTS)
      add_test(NAME weave_postgres_portal_description_libpq COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_portal_description_libpq>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_portal_description_libpq PROPERTIES TIMEOUT 180 LABELS integration)
    endif()
    add_test(NAME weave_postgres_metadata_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_metadata_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_metadata_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_metadata_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()

    add_test(NAME weave_postgres_tls_password_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_tls_password_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_tls_password_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_tls_password_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()

    add_test(NAME weave_postgres_tls_negotiation_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_tls_negotiation_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_tls_negotiation_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_tls_negotiation_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    if(WEAVE_POSTGRES_TLS_LIBPQ_TESTS)
      add_test(NAME weave_postgres_tls_negotiation_libpq COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_tls_negotiation_libpq>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_tls_negotiation_libpq PROPERTIES TIMEOUT 180 LABELS integration)
    endif()
    add_test(NAME weave_postgres_password_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_password_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_password_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_password_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    if(WEAVE_POSTGRES_PASSWORD_LIBPQ_TESTS)
      add_test(NAME weave_postgres_tls_password_libpq COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_tls_password_libpq>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_tls_password_libpq PROPERTIES TIMEOUT 180 LABELS integration)
      add_test(NAME weave_postgres_password_libpq COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/password_libpq.py"
        --executable "$<TARGET_FILE:weave_postgres_password_libpq>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
      set_tests_properties(weave_postgres_password_libpq PROPERTIES TIMEOUT 180 LABELS integration)
    endif()
    if(WEAVE_POSTGRES_KERBEROS_TESTS)
      add_executable(weave_postgres_tls_gss_priority tests/tls_gss_priority.cpp)
      target_include_directories(weave_postgres_tls_gss_priority PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
      target_link_libraries(weave_postgres_tls_gss_priority PRIVATE weave::postgres OpenSSL::Crypto)
      set(tls_priority_runtime)
      if(TARGET weave_runtime)
        target_link_libraries(weave_postgres_tls_gss_priority PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_tls_gss_priority PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
        set(tls_priority_runtime --runtime)
      endif()
      weave_no_exceptions(weave_postgres_tls_gss_priority)
      add_test(NAME weave_postgres_tls_gss_priority COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_gss_priority.py"
        --executable "$<TARGET_FILE:weave_postgres_tls_gss_priority>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}"
        --kdc "${WEAVE_KERBEROS_KDC}" --admin "${WEAVE_KERBEROS_ADMIN}"
        --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}" ${tls_priority_runtime})
      set_tests_properties(weave_postgres_tls_gss_priority PROPERTIES TIMEOUT 120 LABELS integration)

      add_executable(weave_postgres_gss_startup tests/gss_startup.cpp)
      target_include_directories(weave_postgres_gss_startup PRIVATE src)
      target_link_libraries(weave_postgres_gss_startup PRIVATE weave::postgres ${CMAKE_DL_LIBS})
      add_executable(weave_postgres_gss_transport tests/gss_transport.cpp)
      target_include_directories(weave_postgres_gss_transport PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
      target_link_libraries(weave_postgres_gss_transport PRIVATE weave::postgres PkgConfig::WEAVE_GSSAPI ${CMAKE_DL_LIBS}
        OpenSSL::SSL)
      weave_no_exceptions(weave_postgres_gss_transport)
      set(gss_runtime)
      if(TARGET weave_runtime)
        target_link_libraries(weave_postgres_gss_startup PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_gss_startup PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
        target_link_libraries(weave_postgres_gss_transport PRIVATE weave::runtime)
        target_compile_definitions(weave_postgres_gss_transport PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
        set(gss_runtime --runtime)
      endif()
      weave_no_exceptions(weave_postgres_gss_startup)
      add_library(weave_postgres_gss_delay SHARED tests/gss_delay.cpp)
      target_link_libraries(weave_postgres_gss_delay PRIVATE PkgConfig::WEAVE_GSSAPI ${CMAKE_DL_LIBS})
      weave_no_exceptions(weave_postgres_gss_delay)
      set(gss_asan)
      if(WEAVE_ENABLE_ASAN)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libasan.so
          OUTPUT_VARIABLE gss_asan_library OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        if(NOT EXISTS "${gss_asan_library}")
          message(FATAL_ERROR "GSS startup qualification requires the ASan shared runtime")
        endif()
        set(gss_asan --asan "${gss_asan_library}")
      endif()
      add_test(NAME weave_postgres_gss_startup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/gss_startup.py"
        --executable "$<TARGET_FILE:weave_postgres_gss_startup>" --delay "$<TARGET_FILE:weave_postgres_gss_delay>"
        --transport-executable "$<TARGET_FILE:weave_postgres_gss_transport>"
        --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --kdc "${WEAVE_KERBEROS_KDC}" --admin "${WEAVE_KERBEROS_ADMIN}"
        --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}" ${gss_runtime} ${gss_asan})
      set_tests_properties(weave_postgres_gss_startup PROPERTIES TIMEOUT 120 LABELS integration)
      if(WEAVE_POSTGRES_OAUTH_SERVER_TESTS)
        add_executable(weave_postgres_oauth_gss tests/oauth_gss.cpp)
        target_include_directories(weave_postgres_oauth_gss PRIVATE src "${PROJECT_SOURCE_DIR}/test_support")
        target_link_libraries(weave_postgres_oauth_gss PRIVATE weave::postgres PkgConfig::WEAVE_GSSAPI
          OpenSSL::SSL OpenSSL::Crypto)
        if(TARGET weave_runtime)
          target_link_libraries(weave_postgres_oauth_gss PRIVATE weave::runtime)
          target_compile_definitions(weave_postgres_oauth_gss PRIVATE WEAVE_POSTGRES_TEST_RUNTIME=1)
        endif()
        weave_no_exceptions(weave_postgres_oauth_gss)
        add_dependencies(weave_postgres_oauth_gss weave_postgres_oauth_validator)

        # Keep the system libpq functional control separate from Weave's sanitizer process.
        add_executable(weave_postgres_oauth_gss_libpq tests/oauth_gss_libpq.cpp)
        target_compile_features(weave_postgres_oauth_gss_libpq PRIVATE cxx_std_23)
        target_link_libraries(weave_postgres_oauth_gss_libpq PRIVATE PostgreSQL::PostgreSQL)
        weave_no_exceptions(weave_postgres_oauth_gss_libpq)
        add_test(NAME weave_postgres_oauth_gss COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/oauth_gss.py"
          --executable "$<TARGET_FILE:weave_postgres_oauth_gss>"
          --baseline "$<TARGET_FILE:weave_postgres_oauth_gss_libpq>"
          --validator "$<TARGET_FILE:weave_postgres_oauth_validator>"
          --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --kdc "${WEAVE_KERBEROS_KDC}" --admin "${WEAVE_KERBEROS_ADMIN}"
          --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}" ${gss_runtime})
        set_tests_properties(weave_postgres_oauth_gss PROPERTIES TIMEOUT 240 LABELS integration)
      endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(availability_native)
      if(TARGET weave_postgres_availability_live_libpq)
        add_dependencies(weave_postgres_availability_live weave_postgres_availability_live_libpq)
        set(availability_native --baseline "$<TARGET_FILE:weave_postgres_availability_live_libpq>"
          --libpq-version "${result_libpq_version}")
      endif()
      set(availability_gss)
      if(WEAVE_POSTGRES_KERBEROS_TESTS)
        set(availability_gss --kerberos --delay "$<TARGET_FILE:weave_postgres_gss_delay>"
          --kdc "${WEAVE_KERBEROS_KDC}" --admin "${WEAVE_KERBEROS_ADMIN}"
          --database "${WEAVE_KERBEROS_DATABASE}" --kinit "${WEAVE_KERBEROS_KINIT}" ${gss_asan})
      endif()
      add_test(NAME weave_postgres_availability_live COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/availability_live.py"
        --executable "$<TARGET_FILE:weave_postgres_availability_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}"
        ${result_runtime} ${availability_native} ${availability_gss})
      set_tests_properties(weave_postgres_availability_live PROPERTIES TIMEOUT 240 LABELS integration)
    endif()
    add_test(NAME weave_postgres_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --scram-keys)
    set_tests_properties(weave_postgres_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_trace_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_trace_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_trace_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_notifications_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_notifications_live>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_notifications_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_events_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_events_live>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_events_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_result_attach_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_result_attach_live>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_result_attach_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_flush_request_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_flush_request_live>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_flush_request_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_exchange_chunks_live COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py" --executable "$<TARGET_FILE:weave_postgres_exchange_chunks_live>"
      --server-bin "${WEAVE_POSTGRES_SERVER_BIN}")
    set_tests_properties(weave_postgres_exchange_chunks_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_encoding_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_encoding_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --encoding)
    set_tests_properties(weave_postgres_encoding_live PROPERTIES TIMEOUT 180 LABELS integration)
    add_test(NAME weave_postgres_exchange_live COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
      --executable "$<TARGET_FILE:weave_postgres_exchange_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --exchanges)
    set_tests_properties(weave_postgres_exchange_live PROPERTIES TIMEOUT 180 LABELS integration)
    if(WEAVE_ENABLE_ASAN AND MSVC)
      set_tests_properties(weave_postgres_encoding_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_trace_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_notifications_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_events_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_result_attach_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_flush_request_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_exchange_chunks_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
      set_tests_properties(weave_postgres_exchange_live PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WEAVE_COMPILER_BIN}")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME weave_postgres_local_abstract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server.py"
        --executable "$<TARGET_FILE:weave_postgres_live>" --server-bin "${WEAVE_POSTGRES_SERVER_BIN}" --local-abstract)
      set_tests_properties(weave_postgres_local_abstract PROPERTIES TIMEOUT 180 LABELS integration)
    endif()
  endif()
endif()

option(WEAVE_POSTGRES_BENCHMARKS "Build the optional native PostgreSQL/libpq comparison" OFF)
if(WEAVE_POSTGRES_BENCHMARKS)
  set(postgres_comparison_version 18)
  if(WIN32)
    find_package(PostgreSQL ${postgres_comparison_version} REQUIRED)
    set(postgres_comparison PostgreSQL::PostgreSQL)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(POSTGRES_COMPARE REQUIRED IMPORTED_TARGET libpq>=${postgres_comparison_version})
    set(postgres_comparison PkgConfig::POSTGRES_COMPARE)
  endif()
  add_executable(weave_postgres_bench benchmarks/compare.cpp)
  target_link_libraries(weave_postgres_bench PRIVATE weave::postgres ${postgres_comparison})
  weave_no_exceptions(weave_postgres_bench)
  target_compile_definitions(weave_postgres_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
  weave_postgres_runtime(weave_postgres_bench)
  add_executable(weave_postgres_bench_fixture benchmarks/server_fixture.cpp)
  target_include_directories(weave_postgres_bench_fixture PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
  target_link_libraries(weave_postgres_bench_fixture PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_features(weave_postgres_bench_fixture PRIVATE cxx_std_23)
  weave_no_exceptions(weave_postgres_bench_fixture)
  weave_tls_runtime(weave_postgres_bench_fixture)
  if(TARGET weave_runtime)
    add_executable(weave_postgres_concurrent benchmarks/concurrent.cpp)
    target_include_directories(weave_postgres_concurrent PRIVATE "${PROJECT_SOURCE_DIR}/test_support")
    target_link_libraries(weave_postgres_concurrent PRIVATE weave::postgres weave::runtime ${postgres_comparison})
    weave_no_exceptions(weave_postgres_concurrent)
    target_compile_definitions(weave_postgres_concurrent PRIVATE _CRT_SECURE_NO_WARNINGS)
    weave_postgres_runtime(weave_postgres_concurrent)
  endif()
  if(WIN32)
    get_filename_component(postgres_lib_dir "${PostgreSQL_LIBRARY}" DIRECTORY)
    get_filename_component(postgres_prefix "${postgres_lib_dir}" DIRECTORY)
    if(EXISTS "${postgres_prefix}/bin/libpq.dll")
      add_custom_command(TARGET weave_postgres_bench POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${postgres_prefix}/bin/libpq.dll" "$<TARGET_FILE_DIR:weave_postgres_bench>")
      if(TARGET weave_postgres_concurrent)
        add_custom_command(TARGET weave_postgres_concurrent POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${postgres_prefix}/bin/libpq.dll" "$<TARGET_FILE_DIR:weave_postgres_concurrent>")
      endif()
    endif()
  endif()
endif()
