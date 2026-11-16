// The ShardedStore test suite, run unchanged against the lock-free queue (Phase 4, D-021).
//
// The class is a template over the queue type, and sharded_store_test.cpp names it `ShardedStore`.
// Including that file after this macro makes every test in it use LockFreeShardedStore instead,
// so the two queue implementations are held to exactly the same tests. The header is included
// first so the macro does not rename the class definition itself.
//
// One test is not meaningful here: TinyQueuesApplyBackpressure... sets queue_capacity = 1, which
// the unbounded MpscQueue ignores. It still passes (it checks no deadlock and no loss) but it no
// longer exercises backpressure.
#include "lldstore/sharded_store.h"

#define ShardedStore LockFreeShardedStore
#include "sharded_store_test.cpp"
