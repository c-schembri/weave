add_library(weave_database INTERFACE)
weave_module(database)
target_link_libraries(weave_database INTERFACE weave::core)
include("${PROJECT_SOURCE_DIR}/cmake/WeaveMigrations.cmake")
install(FILES "${PROJECT_SOURCE_DIR}/cmake/WeaveMigrations.cmake"
  "${PROJECT_SOURCE_DIR}/cmake/migration_manifest.in"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/weave")

# Build-only support for async drivers; never exported as a consumer component.
if("sync" IN_LIST WEAVE_ENABLED_MODULES)
  add_library(weave_database_async INTERFACE)
  target_include_directories(weave_database_async INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/include")
  target_link_libraries(weave_database_async INTERFACE weave::database weave::sync)
endif()

install(FILES include/weave/database/detail/vendor/pfr/LICENSE_1_0.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/weave/licenses" RENAME boost-pfr-LICENSE)

if(WEAVE_BUILD_TESTS)
  weave_add_test(weave_database_migration_tests tests/migrations.cpp)
  target_link_libraries(weave_database_migration_tests PRIVATE weave::database)
  add_test(NAME weave_migration_generator COMMAND "${CMAKE_COMMAND}"
    "-DWEAVE_SOURCE=${PROJECT_SOURCE_DIR}" "-DWEAVE_WORK=${PROJECT_BINARY_DIR}/migration-generator-tests"
    "-DWEAVE_GENERATOR=${CMAKE_GENERATOR}" "-DWEAVE_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DWEAVE_CONFIG=$<CONFIG>" "-DWEAVE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/generator.cmake")
  set_tests_properties(weave_migration_generator PROPERTIES TIMEOUT 180 LABELS tooling)
  weave_add_test(weave_database_tests tests/database.cpp)
  target_link_libraries(weave_database_tests PRIVATE weave::database)
  if(TARGET weave_database_async)
    weave_add_test(weave_database_admission_tests tests/admission.cpp)
    target_link_libraries(weave_database_admission_tests PRIVATE weave_database_async)
  endif()
endif()
