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

add_executable(itch_tests
  itch/test_codec.cpp
  itch/test_file_reader.cpp)
target_link_libraries(itch_tests PRIVATE lab::itch GTest::gtest_main)
gtest_discover_tests(itch_tests DISCOVERY_TIMEOUT 30)

add_executable(book_tests
  book/test_book_builder.cpp
  book/test_order_book.cpp
  book/test_order_map.cpp)
target_link_libraries(book_tests PRIVATE lab::book GTest::gtest_main)
gtest_discover_tests(book_tests DISCOVERY_TIMEOUT 30)
