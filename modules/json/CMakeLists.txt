add_library(weave_json INTERFACE)
weave_module(json)
target_link_libraries(weave_json INTERFACE weave::core)
target_include_directories(weave_json SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(DIRECTORY vendor/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES vendor/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/weave/licenses" RENAME glaze-LICENSE)

if(WEAVE_BUILD_TESTS)
  weave_add_test(weave_json_tests tests/json.cpp)
  target_link_libraries(weave_json_tests PRIVATE weave::json)
endif()
