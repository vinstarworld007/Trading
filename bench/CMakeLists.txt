add_executable(core_bench bm_clock.cpp bm_histogram.cpp bm_spsc_ring.cpp)
target_link_libraries(core_bench PRIVATE lab::core benchmark::benchmark_main)

add_executable(itch_bench bm_itch.cpp)
target_link_libraries(itch_bench PRIVATE lab::book benchmark::benchmark_main)

add_executable(mold_bench bm_mold.cpp)
target_link_libraries(mold_bench PRIVATE lab::mold benchmark::benchmark_main)

add_executable(match_bench bm_match.cpp)
target_link_libraries(match_bench PRIVATE lab::match benchmark::benchmark_main)
