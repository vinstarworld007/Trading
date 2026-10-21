add_executable(core_bench bm_clock.cpp bm_histogram.cpp bm_spsc_ring.cpp)
target_link_libraries(core_bench PRIVATE lab::core benchmark::benchmark_main)
