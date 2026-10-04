weave_benchmark_group(runtime scheduling.cpp native.cpp)
target_link_libraries(weave_bench_runtime PRIVATE weave::runtime weave_usockets)
