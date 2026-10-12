add_executable(json_struct struct.cpp)
target_link_libraries(json_struct PRIVATE weave::json)
weave_no_exceptions(json_struct)
