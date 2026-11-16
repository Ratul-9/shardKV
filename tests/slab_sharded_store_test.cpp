// The ShardedStore suite against SlabShardedStore (intrusive queue + slab-backed FlatStore).
// Same macro technique as lockfree_sharded_store_test.cpp (D-021).
#include "lldstore/sharded_store.h"

#include <gtest/gtest.h>

#define ShardedStore SlabShardedStore
#include "sharded_store_test.cpp"
