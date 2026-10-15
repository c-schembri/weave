if(NOT TARGET weave_runtime)
  message(FATAL_ERROR "MySQL comparisons require WEAVE_MODULES to include runtime")
endif()
set(WEAVE_MYSQL_NATIVE_ROOT "" CACHE PATH "Native MySQL C API SDK, comparison only")
find_path(WEAVE_MYSQL_NATIVE_INCLUDE mysql.h HINTS "${WEAVE_MYSQL_NATIVE_ROOT}/include" PATH_SUFFIXES mysql REQUIRED)
find_path(WEAVE_MYSQL_NATIVE_HEADER_ROOT mysql/udf_registration_types.h
  HINTS "${WEAVE_MYSQL_NATIVE_ROOT}/include" REQUIRED)
find_library(WEAVE_MYSQL_NATIVE_LIBRARY NAMES libmysql mysqlclient
  HINTS "${WEAVE_MYSQL_NATIVE_ROOT}/lib" "${WEAVE_MYSQL_NATIVE_ROOT}/lib/x86_64-linux-gnu" REQUIRED)
add_executable(weave_mysql_compare compare.cpp)
target_link_libraries(weave_mysql_compare PRIVATE weave::mysql weave::runtime "${WEAVE_MYSQL_NATIVE_LIBRARY}")
target_include_directories(weave_mysql_compare PRIVATE "${WEAVE_MYSQL_NATIVE_INCLUDE}" "${WEAVE_MYSQL_NATIVE_HEADER_ROOT}")
weave_tls_runtime(weave_mysql_compare)
if(WIN32)
  find_file(WEAVE_MYSQL_NATIVE_DLL libmysql.dll HINTS "${WEAVE_MYSQL_NATIVE_ROOT}/lib" REQUIRED)
  add_custom_command(TARGET weave_mysql_compare POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${WEAVE_MYSQL_NATIVE_DLL}" "$<TARGET_FILE_DIR:weave_mysql_compare>")
endif()
