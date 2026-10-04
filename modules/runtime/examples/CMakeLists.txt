add_executable(weave_multicore multicore.cpp)
target_link_libraries(weave_multicore PRIVATE weave::runtime)
weave_no_exceptions(weave_multicore)
set_target_properties(weave_multicore PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
