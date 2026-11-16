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

add_executable(mpsc_queue_tests mpsc_queue_test.cpp)
target_link_libraries(mpsc_queue_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(mpsc_queue_tests)

# The ShardedStore tests again, against the lock-free queue. TEST_PREFIX keeps the ctest names
# distinct from sharded_store_tests, which has the same test names.
add_executable(lockfree_sharded_store_tests lockfree_sharded_store_test.cpp)
target_link_libraries(lockfree_sharded_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(lockfree_sharded_store_tests TEST_PREFIX "lockfree.")

add_executable(flat_store_tests flat_store_test.cpp flat_store_suite_test.cpp)
target_link_libraries(flat_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(flat_store_tests)

add_executable(flat_sharded_store_tests flat_sharded_store_test.cpp)
target_link_libraries(flat_sharded_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(flat_sharded_store_tests TEST_PREFIX "flat.")

# Phase 6: memory management
add_executable(slab_allocator_tests slab_allocator_test.cpp)
target_link_libraries(slab_allocator_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(slab_allocator_tests)

add_executable(intrusive_mpsc_queue_tests intrusive_mpsc_queue_test.cpp)
target_link_libraries(intrusive_mpsc_queue_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(intrusive_mpsc_queue_tests)

add_executable(slab_sharded_store_tests slab_sharded_store_test.cpp)
target_link_libraries(slab_sharded_store_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(slab_sharded_store_tests TEST_PREFIX "slab.")

# Replaces global operator new, so it is a separate executable.
add_executable(zero_alloc_tests zero_alloc_test.cpp)
target_link_libraries(zero_alloc_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(zero_alloc_tests)

# Phase 7: durability
add_executable(durability_tests durability_test.cpp)
target_link_libraries(durability_tests PRIVATE lldstore_core GTest::gtest_main)
gtest_discover_tests(durability_tests)

# Phase 8: replication (Raft) in the deterministic simulator
add_executable(raft_tests raft_test.cpp)
target_link_libraries(raft_tests PRIVATE lldstore_core GTest::gtest_main)
gtest_discover_tests(raft_tests)

# Phase 10: linearizability checker and chaos runs
add_executable(linearizability_tests linearizability_test.cpp)
target_link_libraries(linearizability_tests PRIVATE lldstore_core GTest::gtest_main)
gtest_discover_tests(linearizability_tests)

add_executable(chaos_tests chaos_test.cpp)
target_link_libraries(chaos_tests PRIVATE lldstore_core GTest::gtest_main)
gtest_discover_tests(chaos_tests PROPERTIES TIMEOUT 1800)

# Phase 8 end to end: three KvNodes over localhost TCP
add_executable(cluster_tests cluster_test.cpp)
target_link_libraries(cluster_tests PRIVATE lldstore_core GTest::gtest_main)
gtest_discover_tests(cluster_tests PROPERTIES RUN_SERIAL TRUE)

# Phase 9: epoch-based reclamation
add_executable(epoch_tests epoch_test.cpp)
target_link_libraries(epoch_tests PRIVATE lldstore_common GTest::gtest_main)
gtest_discover_tests(epoch_tests)
