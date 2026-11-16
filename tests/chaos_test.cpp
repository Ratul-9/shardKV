// Phase 10: hundreds of seeded chaos runs of the replicated store in the deterministic simulator.
// Each run injects message loss, delays/reordering, partitions and node crashes/restarts while
// clients run, then checks Raft's safety properties, convergence, and the linearizability of
// everything the clients observed. A failing seed is printed and reproduces the run exactly:
//     LLDSTORE_CHAOS_SEED=<seed> ./chaos_tests --gtest_filter='*Single*'
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "lldstore/linearizability.h"
#include "raft_sim.h"

namespace lldstore {
namespace {

using sim::Cluster;
using sim::SimOptions;

struct RunSummary {
  bool ok = true;
  std::string failure;
  size_t operations = 0;
  size_t unknown = 0;
  uint64_t crashes = 0;
  uint64_t partitions = 0;
};

RunSummary RunChaos(uint64_t seed, int nodes) {
  SimOptions o;
  o.seed = seed;
  o.nodes = nodes;
  o.clients = 4;
  o.keys = 3;
  o.drop_probability = 0.05;
  o.min_delay = 1;
  o.max_delay = 6;  // up to 6 ticks: messages overtake each other
  o.crash_probability = 0.004;
  o.partition_probability = 0.004;
  o.heal_probability = 0.02;
  Cluster cluster(o);
  cluster.Run(3000);
  cluster.StopClients();
  cluster.Quiesce(600);

  RunSummary s;
  s.crashes = cluster.crashes();
  s.partitions = cluster.partitions();
  s.operations = cluster.history().size();
  for (const auto& op : cluster.history()) s.unknown += op.known ? 0 : 1;
  if (!cluster.violations().empty()) {
    s.ok = false;
    s.failure = cluster.violations().front();
    return s;
  }
  std::string why;
  if (!cluster.Converged(&why)) {
    s.ok = false;
    s.failure = "did not converge: " + why;
    return s;
  }
  const lin::CheckResult r = lin::Check(cluster.history());
  if (r.verdict != lin::Verdict::kLinearizable) {
    s.ok = false;
    s.failure = std::string("linearizability: ") + lin::VerdictName(r.verdict) + " on key " + r.key;
  }
  return s;
}

TEST(Chaos, ThreeNodesManySeeds) {
  int failures = 0;
  size_t operations = 0, unknown = 0;
  uint64_t crashes = 0, partitions = 0;
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    const RunSummary s = RunChaos(seed, 3);
    operations += s.operations;
    unknown += s.unknown;
    crashes += s.crashes;
    partitions += s.partitions;
    if (!s.ok) {
      ++failures;
      ADD_FAILURE() << "seed " << seed << ": " << s.failure;
    }
  }
  // The faults must actually have happened, or "zero violations" means nothing.
  EXPECT_GT(crashes, 100u);
  EXPECT_GT(partitions, 100u);
  EXPECT_GT(operations, 10000u);
  std::printf("[chaos] 200 runs, %zu ops (%zu unknown), %llu crashes, %llu partitions, %d failed\n",
              operations, unknown, static_cast<unsigned long long>(crashes),
              static_cast<unsigned long long>(partitions), failures);
}

TEST(Chaos, FiveNodesManySeeds) {
  for (uint64_t seed = 1001; seed <= 1100; ++seed) {
    const RunSummary s = RunChaos(seed, 5);
    EXPECT_TRUE(s.ok) << "seed " << seed << ": " << s.failure;
  }
}

TEST(Chaos, SingleSeedFromEnvironment) {
  const char* env = std::getenv("LLDSTORE_CHAOS_SEED");
  if (env == nullptr) GTEST_SKIP() << "set LLDSTORE_CHAOS_SEED to replay one run";
  const RunSummary s = RunChaos(std::strtoull(env, nullptr, 10), 3);
  EXPECT_TRUE(s.ok) << s.failure;
}

}  // namespace
}  // namespace lldstore
