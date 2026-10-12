foreach(source IN ITEMS queries/borrowed.cpp queries/primitives.cpp queries/struct.cpp
    queries/simple_struct.cpp queries/shared_mapping.cpp connections/pool.cpp)
  get_filename_component(example ${source} NAME_WE)
  add_executable(postgres_${example} ${source})
  target_link_libraries(postgres_${example} PRIVATE weave::postgres)
  weave_no_exceptions(postgres_${example})
  set_target_properties(postgres_${example} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
  weave_postgres_runtime(postgres_${example})
endforeach()

add_executable(postgres_migrations migrations/main.cpp)
target_link_libraries(postgres_migrations PRIVATE weave::postgres)
weave_no_exceptions(postgres_migrations)
weave_postgres_runtime(postgres_migrations)
set_target_properties(postgres_migrations PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
weave_add_migrations(TARGET postgres_migrations
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/migrations/changes" NAME app_migrations)
