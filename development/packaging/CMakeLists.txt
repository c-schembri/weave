include(CMakePackageConfigHelpers)

configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/weave-config.cmake.in"
  "${PROJECT_BINARY_DIR}/weave-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/weave-config-version.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/weave-config.cmake"
  "${PROJECT_BINARY_DIR}/weave-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")

if(WEAVE_BUILD_TESTS)
  set(package_cases ${WEAVE_ENABLED_MODULES})
  if("tcp" IN_LIST WEAVE_ENABLED_MODULES AND "runtime" IN_LIST WEAVE_ENABLED_MODULES)
    list(APPEND package_cases all)
  endif()
  foreach(component IN LISTS package_cases)
    add_test(NAME package_components_${component} COMMAND "${CMAKE_COMMAND}"
      "-DWEAVE_SOURCE=${PROJECT_SOURCE_DIR}"
      "-DWEAVE_WORK=${PROJECT_BINARY_DIR}/package-tests"
      "-DWEAVE_GENERATOR=${CMAKE_GENERATOR}"
      "-DWEAVE_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DWEAVE_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
      "-DWEAVE_CONFIG=$<CONFIG>"
      "-DWEAVE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DWEAVE_TEST_MODULES=${component}"
      "-DWEAVE_ASAN=${WEAVE_ENABLE_ASAN}"
      "-DWEAVE_PACKAGE_TLS=$<TARGET_EXISTS:weave_tls>"
      "-DWEAVE_PACKAGE_POSTGRES=$<TARGET_EXISTS:weave_postgres>"
      "-DWEAVE_PACKAGE_MYSQL=$<TARGET_EXISTS:weave_mysql>"
      "-DWEAVE_POSTGRES_GSSAPI=${WEAVE_POSTGRES_GSSAPI}"
      "-DWEAVE_POSTGRES_LDAP=${WEAVE_POSTGRES_LDAP}"
      "-DWeaveLDAP_INCLUDE_DIR=${WeaveLDAP_INCLUDE_DIR}"
      "-DWeaveLDAP_LIBRARY=${WeaveLDAP_LIBRARY}"
      "-DWEAVE_PACKAGE_LOCAL=$<TARGET_EXISTS:weave_local>"
      "-DICU_ROOT=${ICU_ROOT}"
      "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
      "-DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}"
      "-DWEAVE_TLS_RUNTIME_DIR=${WEAVE_TLS_RUNTIME_DIR}"
      -P "${PROJECT_SOURCE_DIR}/development/packaging/tests/components.cmake")
    set(package_timeout 180)
    if(component STREQUAL "postgres" OR (component STREQUAL "all" AND TARGET weave_postgres))
      set(package_timeout 600)
    endif()
    set_tests_properties(package_components_${component} PROPERTIES TIMEOUT ${package_timeout} LABELS packaging)
  endforeach()
endif()
