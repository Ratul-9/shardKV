#include "lldstore/linearizability.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/locked_store.h"
#include "lldstore/sharded_store.h"

namespace lldstore {
namespace {

using lin::Check;
using lin::Operation;
using lin::OpType;
using lin::Verdict;

Operation Put(uint64_t client, std::string key, std::string value, int64_t call, int64_t ret) {
  Operation op;
  op.client = client;
  op.type = OpType::kPut;
  op.key = std::move(key);
  op.value = std::move(value);
  op.call = call;
  op.ret = ret;
  return op;
}

Operation Get(uint64_t client, std::string key, std::optional<std::string> out, int64_t call, int64_t ret) {
  Operation op;
  op.client = client;
  op.type = OpType::kGet;
  op.key = std::move(key);
  op.found = out.has_value();
  if (out) op.output = *out;
  op.call = call;
  op.ret = ret;
  return op;
}

Operation Del(uint64_t client, std::string key, bool existed, int64_t call, int64_t ret) {
  Operation op;
  op.client = client;
  op.type = OpType::kDelete;
  op.key = std::move(key);
  op.found = existed;
  op.call = call;
  op.ret = ret;
  return op;
}

Operation Unknown(Operation op) {
  op.known = false;
  op.ret = lin::kNoReturn;
  return op;
}

// ---- The checker must accept what is legal ... ----------------------------------------------

TEST(Linearizability, EmptyAndSequentialHistoriesPass) {
  EXPECT_EQ(Check({}).verdict, Verdict::kLinearizable);
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 1), Get(1, "x", "1", 2, 3), Del(1, "x", true, 4, 5),
                   Get(1, "x", std::nullopt, 6, 7), Del(1, "x", false, 8, 9)})
                .verdict,
            Verdict::kLinearizable);
}

TEST(Linearizability, AReadOverlappingAWriteMaySeeEitherValue) {
  // put(x,1) done; then put(x,2) [10,20] overlaps two reads that see 1 and 2.
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 5), Put(1, "x", "2", 10, 20), Get(2, "x", "1", 11, 12),
                   Get(3, "x", "2", 13, 14)})
                .verdict,
            Verdict::kLinearizable);
}

TEST(Linearizability, ConcurrentOpsCanBeOrderedEitherWay) {
  EXPECT_EQ(Check({Put(1, "x", "a", 0, 10), Put(2, "x", "b", 0, 10), Get(3, "x", "a", 11, 12)}).verdict,
            Verdict::kLinearizable);
  EXPECT_EQ(Check({Put(1, "x", "a", 0, 10), Put(2, "x", "b", 0, 10), Get(3, "x", "b", 11, 12)}).verdict,
            Verdict::kLinearizable);
}

TEST(Linearizability, AnUnknownWriteMayHaveHappenedOrNot) {
  const Operation maybe = Unknown(Put(1, "x", "1", 0, 0));
  EXPECT_EQ(Check({maybe, Get(2, "x", "1", 5, 6)}).verdict, Verdict::kLinearizable);
  EXPECT_EQ(Check({maybe, Get(2, "x", std::nullopt, 5, 6)}).verdict, Verdict::kLinearizable);
  // ... but only after it was invoked.
  EXPECT_EQ(Check({Get(2, "x", "1", 0, 1), Unknown(Put(1, "x", "1", 5, 0))}).verdict, Verdict::kViolation);
}

// ---- ... and reject what is not (a checker that accepts everything is worse than none) ------

TEST(Linearizability, StaleReadAfterACompletedWriteIsAViolation) {
  const auto r = Check({Put(1, "x", "1", 0, 1), Put(1, "x", "2", 2, 3), Get(2, "x", "1", 4, 5)});
  EXPECT_EQ(r.verdict, Verdict::kViolation);
  EXPECT_EQ(r.key, "x");
}

TEST(Linearizability, ReadingAValueNeverWrittenIsAViolation) {
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 1), Get(2, "x", "7", 2, 3)}).verdict, Verdict::kViolation);
}

TEST(Linearizability, LostWriteIsAViolation) {
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 1), Get(2, "x", std::nullopt, 2, 3)}).verdict, Verdict::kViolation);
}

TEST(Linearizability, NonMonotonicReadsAreAViolation) {
  // Two sequential reads by one client during a long write: once it has seen the new value, it
  // cannot see the old one again.
  EXPECT_EQ(Check({Put(1, "x", "old", 0, 1), Put(2, "x", "new", 2, 100), Get(3, "x", "new", 3, 4),
                   Get(3, "x", "old", 5, 6)})
                .verdict,
            Verdict::kViolation);
}

TEST(Linearizability, DeleteResultsAreChecked) {
  EXPECT_EQ(Check({Del(1, "x", true, 0, 1)}).verdict, Verdict::kViolation);
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 1), Del(1, "x", true, 2, 3), Del(2, "x", true, 4, 5)}).verdict,
            Verdict::kViolation);
}

TEST(Linearizability, KeysAreIndependent) {
  EXPECT_EQ(Check({Put(1, "x", "1", 0, 1), Get(2, "y", std::nullopt, 2, 3)}).verdict,
            Verdict::kLinearizable);
}

TEST(Linearizability, HistoryFilesRoundTrip) {
  std::vector<Operation> h = {Put(1, "a b", std::string("\0\xff", 2), 0, 1), Get(2, "a b", std::nullopt, 2, 3),
                              Unknown(Del(3, "k", false, 4, 0))};
  std::vector<Operation> back;
  std::string error;
  ASSERT_TRUE(lin::ParseHistory(lin::FormatHistory(h), &back, &error)) << error;
  ASSERT_EQ(back.size(), h.size());
  EXPECT_EQ(back[0].value, h[0].value);
  EXPECT_EQ(back[0].key, "a b");
  EXPECT_FALSE(back[2].known);
  EXPECT_FALSE(lin::ParseHistory("1 0 5 3 1 0 61 - -\n", &back, &error));  // ret < call
}

// ---- Randomised: generated from a real sequential execution, so known to be linearizable; then
//      one read is corrupted, so known not to be. --------------------------------------------

TEST(LinearizabilityRandomised, SequentialRunsPassAndCorruptedRunsFail) {
  std::mt19937_64 rng(3);
  for (int round = 0; round < 200; ++round) {
    std::map<std::string, std::string> model;
    std::vector<Operation> h;
    int64_t t = 0;
    for (int i = 0; i < 60; ++i) {
      const std::string key = "k" + std::to_string(rng() % 3);
      const uint64_t r = rng() % 3;
      // Overlap neighbouring ops a little: each op's interval contains its own instant.
      const int64_t call = t - static_cast<int64_t>(rng() % 3), ret = t + static_cast<int64_t>(rng() % 3);
      if (r == 0) {
        const std::string v = std::to_string(i);
        model[key] = v;
        h.push_back(Put(1, key, v, call, ret));
      } else if (r == 1) {
        auto it = model.find(key);
        h.push_back(Get(2, key, it == model.end() ? std::nullopt : std::optional<std::string>(it->second), call, ret));
      } else {
        h.push_back(Del(3, key, model.erase(key) == 1, call, ret));
      }
      t += 10;
    }
    ASSERT_EQ(Check(h).verdict, Verdict::kLinearizable) << round;
    for (Operation& op : h) {
      if (op.type == OpType::kGet) {
        op.found = true;
        op.output = "never-written";
        break;
      }
    }
    ASSERT_EQ(Check(h).verdict, Verdict::kViolation) << round;
  }
}

// ---- Real concurrent stores, recorded and checked -------------------------------------------

template <typename S>
std::vector<Operation> RecordConcurrentHistory(S& store, int threads, int ops) {
  std::vector<std::vector<Operation>> per(static_cast<size_t>(threads));
  const auto start = std::chrono::steady_clock::now();
  auto now = [&] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  };
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<uint64_t>(t) + 100);
      std::string out;
      for (int i = 0; i < ops; ++i) {
        const std::string key = "k" + std::to_string(rng() % 4);
        Operation op;
        op.client = static_cast<uint64_t>(t);
        op.key = key;
        const uint64_t r = rng() % 3;
        op.call = now();
        if (r == 0) {
          op.type = OpType::kPut;
          op.value = std::to_string(t) + "-" + std::to_string(i);
          store.Put(key, op.value);
        } else if (r == 1) {
          op.type = OpType::kGet;
          op.found = store.Get(key, &out);
          if (op.found) op.output = out;
        } else {
          op.type = OpType::kDelete;
          op.found = store.Delete(key);
        }
        op.ret = now();
        per[static_cast<size_t>(t)].push_back(op);
      }
    });
  }
  for (auto& w : workers) w.join();
  std::vector<Operation> all;
  for (auto& v : per) all.insert(all.end(), v.begin(), v.end());
  return all;
}

TEST(LinearizabilityOfStores, MutexStoreIsLinearizable) {
  MutexStore store;
  EXPECT_EQ(Check(RecordConcurrentHistory(store, 4, 500)).verdict, Verdict::kLinearizable);
}

TEST(LinearizabilityOfStores, ShardedStoresAreLinearizable) {
  ShardedStoreOptions options;
  options.num_shards = 3;
  {
    ShardedStore store(options);
    EXPECT_EQ(Check(RecordConcurrentHistory(store, 4, 500)).verdict, Verdict::kLinearizable);
  }
  {
    SlabShardedStore store(options);
    EXPECT_EQ(Check(RecordConcurrentHistory(store, 4, 500)).verdict, Verdict::kLinearizable);
  }
}

// A store that is deliberately NOT linearizable: each thread caches what it last wrote and
// returns it from Get without asking the store. The checker must catch it.
TEST(LinearizabilityOfStores, ABrokenStoreIsCaught) {
  struct StaleCache {
    MutexStore store;
    std::mutex mu;
    std::map<std::thread::id, std::map<std::string, std::string>> mine;
    void Put(std::string_view k, std::string_view v) {
      store.Put(k, v);
      std::lock_guard<std::mutex> lock(mu);
      mine[std::this_thread::get_id()][std::string(k)] = std::string(v);
    }
    bool Get(std::string_view k, std::string* out) {
      {
        std::lock_guard<std::mutex> lock(mu);
        auto& m = mine[std::this_thread::get_id()];
        auto it = m.find(std::string(k));
        if (it != m.end()) {
          *out = it->second;
          return true;
        }
      }
      return store.Get(k, out);
    }
    bool Delete(std::string_view k) { return store.Delete(k); }
  } broken;
  EXPECT_EQ(Check(RecordConcurrentHistory(broken, 4, 400)).verdict, Verdict::kViolation);
}

}  // namespace
}  // namespace lldstore
