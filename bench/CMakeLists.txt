add_executable(histogram_bench histogram_bench.cpp)
target_link_libraries(histogram_bench PRIVATE lldstore_common benchmark::benchmark benchmark::benchmark_main)

add_executable(store_bench store_bench.cpp)
target_link_libraries(store_bench PRIVATE lldstore_common benchmark::benchmark benchmark::benchmark_main)

add_executable(store_latency store_latency.cpp)
target_link_libraries(store_latency PRIVATE lldstore_common benchmark::benchmark)

add_executable(footprint footprint.cpp)
target_link_libraries(footprint PRIVATE lldstore_common)
