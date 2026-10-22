include(GoogleTest)

add_executable(core_tests
  core/test_affinity.cpp
  core/test_clock.cpp
  core/test_endian.cpp
  core/test_histogram.cpp
  core/test_huge_pages.cpp
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
  book/test_scenarios.cpp
  book/test_order_map.cpp)
target_link_libraries(book_tests PRIVATE lab::book GTest::gtest_main)
gtest_discover_tests(book_tests DISCOVERY_TIMEOUT 30)

add_executable(mold_tests
  mold/test_fault_injector.cpp
  mold/test_mold.cpp
  mold/test_pipeline.cpp
  mold/test_retransmit_store.cpp
  mold/test_sequencer.cpp)
target_link_libraries(mold_tests PRIVATE lab::mold lab::book GTest::gtest_main)
gtest_discover_tests(mold_tests DISCOVERY_TIMEOUT 30)

add_executable(net_tests
  net/test_tcp.cpp
  net/test_udp.cpp)
target_link_libraries(net_tests PRIVATE lab::net GTest::gtest_main)
gtest_discover_tests(net_tests DISCOVERY_TIMEOUT 30)

add_executable(match_tests
  match/test_engine.cpp)
target_link_libraries(match_tests PRIVATE lab::match GTest::gtest_main)
gtest_discover_tests(match_tests DISCOVERY_TIMEOUT 30)

add_executable(soup_tests
  soup/test_soup.cpp)
target_link_libraries(soup_tests PRIVATE lab::soup GTest::gtest_main)
gtest_discover_tests(soup_tests DISCOVERY_TIMEOUT 30)

add_executable(ouch_tests
  ouch/test_codec.cpp)
target_link_libraries(ouch_tests PRIVATE lab::ouch GTest::gtest_main)
gtest_discover_tests(ouch_tests DISCOVERY_TIMEOUT 30)

add_executable(firm_tests
  firm/test_market_maker.cpp
  firm/test_oms.cpp
  firm/test_risk.cpp)
target_link_libraries(firm_tests PRIVATE lab::strategy lab::risk lab::oms GTest::gtest_main)
gtest_discover_tests(firm_tests DISCOVERY_TIMEOUT 30)

add_executable(fix_tests
  fix/test_codec.cpp
  fix/test_session.cpp)
target_link_libraries(fix_tests PRIVATE lab::fix GTest::gtest_main)
gtest_discover_tests(fix_tests DISCOVERY_TIMEOUT 30)

add_executable(vendor_tests
  vendor/test_consolidator.cpp
  vendor/test_icf.cpp
  vendor/test_omm_bpipe.cpp
  vendor/test_ws_json.cpp)
target_link_libraries(vendor_tests PRIVATE lab::vendor GTest::gtest_main)
gtest_discover_tests(vendor_tests DISCOVERY_TIMEOUT 30)
