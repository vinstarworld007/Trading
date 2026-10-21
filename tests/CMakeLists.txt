include(GoogleTest)

add_executable(core_tests
  core/test_affinity.cpp
  core/test_clock.cpp
  core/test_endian.cpp
  core/test_histogram.cpp
  core/test_log.cpp
  core/test_shm.cpp
  core/test_spsc_ring.cpp)
target_link_libraries(core_tests PRIVATE lab::core GTest::gtest_main)
gtest_discover_tests(core_tests DISCOVERY_TIMEOUT 30)
