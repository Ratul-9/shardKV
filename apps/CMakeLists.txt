# Phase 8-11 programs: the cluster node, the client/load generator, the history checker.
add_executable(kvnode kvnode.cpp)
target_link_libraries(kvnode PRIVATE lldstore_core)

add_executable(kvctl kvctl.cpp)
target_link_libraries(kvctl PRIVATE lldstore_core)

add_executable(lincheck lincheck.cpp)
target_link_libraries(lincheck PRIVATE lldstore_core)
