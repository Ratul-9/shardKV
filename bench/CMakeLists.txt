add_executable(histogram_bench histogram_bench.cpp)
target_link_libraries(histogram_bench PRIVATE lldstore_common benchmark::benchmark benchmark::benchmark_main)

add_executable(store_bench store_bench.cpp)
target_link_libraries(store_bench PRIVATE lldstore_common benchmark::benchmark benchmark::benchmark_main)

add_executable(store_latency store_latency.cpp)
target_link_libraries(store_latency PRIVATE lldstore_common benchmark::benchmark)

add_executable(footprint footprint.cpp)
target_link_libraries(footprint PRIVATE lldstore_common)

add_executable(contention contention.cpp)
target_link_libraries(contention PRIVATE lldstore_common benchmark::benchmark)

add_executable(table_bench table_bench.cpp)
target_link_libraries(table_bench PRIVATE lldstore_common benchmark::benchmark benchmark::benchmark_main)

# gRPC server and load generator (only when gRPC/Protobuf were found and this is not a sanitizer build)
if(LLDSTORE_HAVE_GRPC)
  add_executable(kv_server kv_server.cpp)
  target_link_libraries(kv_server PRIVATE lldstore_common lldstore_proto)

  add_executable(kv_client kv_client.cpp)
  target_link_libraries(kv_client PRIVATE lldstore_common lldstore_proto benchmark::benchmark)
endif()

# Phase 7: group commit on the real disk
add_executable(wal_bench wal_bench.cpp)
target_link_libraries(wal_bench PRIVATE lldstore_core)

# Phase 9: lock-free reads with epoch reclamation vs reader-writer locks
add_executable(rcu_bench rcu_bench.cpp)
target_link_libraries(rcu_bench PRIVATE lldstore_common)
