weave_benchmark_group(core tasks.cpp calls.cpp)
# IO is only a dependency of these comparison executables, not weave::core.
target_link_libraries(weave_bench_core PRIVATE weave::io)
