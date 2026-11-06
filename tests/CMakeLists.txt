include(GoogleTest)

add_executable(common_tests
  latency_histogram_test.cpp
  open_loop_test.cpp
  multi_client_test.cpp)
target_link_libraries(common_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(common_tests)

add_executable(store_tests store_test.cpp)
target_link_libraries(store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(store_tests)

add_executable(locked_store_tests locked_store_test.cpp)
target_link_libraries(locked_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(locked_store_tests)

add_executable(thread_pool_tests thread_pool_test.cpp)
target_link_libraries(thread_pool_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(thread_pool_tests)

add_executable(bounded_queue_tests bounded_queue_test.cpp)
target_link_libraries(bounded_queue_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(bounded_queue_tests)

add_executable(sharded_store_tests sharded_store_test.cpp)
target_link_libraries(sharded_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(sharded_store_tests)
