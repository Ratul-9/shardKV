#include "lldstore/thread_pool.h"

#include <gtest/gtest.h>

namespace lldstore {
namespace {

// TODO(Ratul): write the ThreadPool tests (run them under the tsan preset too). Cases worth covering:
//  - Every submitted task runs exactly once (count with std::atomic)
//  - Tasks run on more than one thread when num_threads > 1 (collect std::this_thread::get_id())
//  - Destruction with queued tasks: whatever your shutdown rule is, test that it holds and does not hang
//  - Submitting from inside a running task
//  - Many producers submitting concurrently
//  - A task that blocks does not stop other workers from running other tasks
//  - Destroying an idle pool returns promptly (no lost wakeup, no deadlock)
//  - Stress: tens of thousands of tiny tasks from several threads, repeated a few times
TEST(ThreadPool, Placeholder) { GTEST_SKIP() << "ThreadPool tests not written yet"; }

}  // namespace
}  // namespace lldstore
