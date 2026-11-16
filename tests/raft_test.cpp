#include "lldstore/raft.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "raft_sim.h"
#include "test_dir.h"

namespace lldstore {
namespace {

using raft::Entry;
using raft::Message;
using raft::MessageType;
using raft::Role;
using sim::Cluster;
using sim::SimOptions;

TEST(RaftCodec, EveryMessageTypeRoundTrips) {
  std::vector<Message> messages(4);
  messages[0].type = MessageType::kVoteRequest;
  messages[0].last_log_index = 7;
  messages[0].last_log_term = 3;
  messages[1].type = MessageType::kVoteReply;
  messages[1].granted = true;
  messages[2].type = MessageType::kAppend;
  messages[2].prev_index = 4;
  messages[2].prev_term = 2;
  messages[2].commit = 3;
  messages[2].entries = {Entry{2, "a"}, Entry{3, {}}, Entry{3, std::string(1000, 'x')}};
  messages[3].type = MessageType::kAppendReply;
  messages[3].success = true;
  messages[3].match_index = 9;
  for (Message& m : messages) {
    m.from = 2;
    m.to = 3;
    m.term = 5;
    Message back;
    ASSERT_TRUE(raft::DecodeMessage(raft::EncodeMessage(m), &back));
    EXPECT_EQ(raft::EncodeMessage(back), raft::EncodeMessage(m));
  }
  Message junk;
  EXPECT_FALSE(raft::DecodeMessage("", &junk));
  EXPECT_FALSE(raft::DecodeMessage(std::string(3, '\x03'), &junk));
  const std::string good = raft::EncodeMessage(messages[2]);
  for (size_t cut = 0; cut < good.size(); ++cut) {
    EXPECT_FALSE(raft::DecodeMessage(good.substr(0, cut), &junk)) << cut;
  }
}

TEST(RaftNode, ASingleNodeElectsItselfAndCommitsAlone) {
  raft::MemoryStorage storage;
  raft::Config config;
  config.id = 1;
  raft::RaftNode node(config, &storage);
  for (int i = 0; i < 25 && node.role() != Role::kLeader; ++i) node.Tick();
  ASSERT_EQ(node.role(), Role::kLeader);
  const uint64_t index = node.Propose("x");
  EXPECT_EQ(node.commit_index(), index);
  const auto committed = node.TakeCommitted();
  ASSERT_EQ(committed.size(), 2u);  // the leader's no-op, then "x"
  EXPECT_EQ(committed[1].second.data, "x");
}

TEST(RaftNode, RejectsBadTimeouts) {
  raft::MemoryStorage storage;
  raft::Config config;
  config.heartbeat_ticks = 10;
  config.election_ticks_min = 10;
  EXPECT_THROW((void)raft::RaftNode(config, &storage), std::invalid_argument);
}

TEST(RaftCluster, ElectsExactlyOneLeader) {
  Cluster cluster(SimOptions{});
  cluster.Run(100);
  const int leader = cluster.Leader();
  ASSERT_GE(leader, 0);
  int leaders = 0;
  for (int i = 0; i < 3; ++i) leaders += cluster.node(i)->role() == Role::kLeader ? 1 : 0;
  EXPECT_EQ(leaders, 1);
  EXPECT_TRUE(cluster.violations().empty());
}

TEST(RaftCluster, ReplicatesAndCommitsOnEveryNode) {
  Cluster cluster(SimOptions{});
  cluster.Run(100);
  const int leader = cluster.Leader();
  ASSERT_GE(leader, 0);
  for (int i = 0; i < 20; ++i) ASSERT_GT(cluster.ProposeOn(leader, "e" + std::to_string(i)), 0u);
  cluster.Run(50);
  std::string why;
  EXPECT_TRUE(cluster.Converged(&why)) << why;
  for (int i = 0; i < 3; ++i) EXPECT_EQ(cluster.node(i)->commit_index(), cluster.node(leader)->last_index());
  EXPECT_TRUE(cluster.violations().empty());
}

TEST(RaftCluster, ANonLeaderRefusesProposals) {
  Cluster cluster(SimOptions{});
  cluster.Run(100);
  const int leader = cluster.Leader();
  ASSERT_GE(leader, 0);
  EXPECT_EQ(cluster.ProposeOn((leader + 1) % 3, "x"), 0u);
}

TEST(RaftCluster, KillingTheLeaderKeepsEveryCommittedEntry) {
  Cluster cluster(SimOptions{});
  cluster.Run(100);
  const int old_leader = cluster.Leader();
  ASSERT_GE(old_leader, 0);
  for (int i = 0; i < 10; ++i) cluster.ProposeOn(old_leader, "before" + std::to_string(i));
  cluster.Run(30);
  const uint64_t committed = cluster.node(old_leader)->commit_index();
  cluster.Crash(old_leader);
  cluster.Run(100);
  const int new_leader = cluster.Leader();
  ASSERT_GE(new_leader, 0);
  ASSERT_NE(new_leader, old_leader);
  EXPECT_GT(cluster.node(new_leader)->term(), 0u);
  cluster.ProposeOn(new_leader, "after");
  cluster.Quiesce(150);
  std::string why;
  ASSERT_TRUE(cluster.Converged(&why)) << why;
  // Every entry committed under the old leader is still at the same index on every node.
  for (int i = 0; i < 3; ++i) {
    for (uint64_t idx = 1; idx <= committed; ++idx) {
      EXPECT_EQ(cluster.storage(i).entry(idx), cluster.storage(old_leader).entry(idx)) << i << " " << idx;
    }
  }
  EXPECT_TRUE(cluster.violations().empty());
}

TEST(RaftCluster, AMinorityLeaderCannotCommitAndStepsDown) {
  Cluster cluster(SimOptions{});
  cluster.Run(100);
  const int leader = cluster.Leader();
  ASSERT_GE(leader, 0);
  cluster.Partition({leader});  // the leader alone on one side
  const uint64_t before = cluster.node(leader)->commit_index();
  cluster.ProposeOn(leader, "lost?");
  cluster.Run(60);
  EXPECT_EQ(cluster.node(leader)->commit_index(), before);  // no majority, no commit
  EXPECT_NE(cluster.node(leader)->role(), Role::kLeader);  // check-quorum stepped it down
  const int majority_leader = cluster.Leader();
  ASSERT_GE(majority_leader, 0);
  EXPECT_NE(majority_leader, leader);
  cluster.ProposeOn(majority_leader, "kept");
  cluster.Quiesce(150);
  std::string why;
  ASSERT_TRUE(cluster.Converged(&why)) << why;
  EXPECT_TRUE(cluster.violations().empty());
}

TEST(RaftCluster, AStaleFollowerIsRepairedAfterThePartitionHeals) {
  SimOptions options;
  options.nodes = 5;
  Cluster cluster(options);
  cluster.Run(100);
  int leader = cluster.Leader();
  ASSERT_GE(leader, 0);
  const int stale = (leader + 1) % 5;
  cluster.Partition({stale});
  for (int i = 0; i < 300; ++i) cluster.ProposeOn(leader, "x" + std::to_string(i));  // > one message
  cluster.Run(50);
  cluster.Quiesce(300);
  std::string why;
  ASSERT_TRUE(cluster.Converged(&why)) << why;
  EXPECT_TRUE(cluster.violations().empty());
}

TEST(RaftFileStorage, StateSurvivesReopenAndTornTailsAreCut) {
  testing::TempDir dir;
  {
    raft::FileStorage s(dir.path(), SyncMode::kNone);
    s.SetTermAndVote(4, 2);
    s.Append({Entry{1, "a"}, Entry{2, "b"}, Entry{4, "c"}});
    s.TruncateFrom(3);
    s.Append({Entry{4, "d"}});
  }
  {
    raft::FileStorage s(dir.path(), SyncMode::kNone);
    EXPECT_EQ(s.term(), 4u);
    EXPECT_EQ(s.voted_for(), 2u);
    ASSERT_EQ(s.last_index(), 3u);
    EXPECT_EQ(s.entry(3), (Entry{4, "d"}));
    EXPECT_EQ(s.term_at(2), 2u);
  }
  std::filesystem::resize_file(dir.File("raft-log"), std::filesystem::file_size(dir.File("raft-log")) - 1);
  raft::FileStorage s(dir.path(), SyncMode::kNone);
  EXPECT_EQ(s.last_index(), 2u);  // the torn "d" append is gone; the truncate before it stands
  s.Append({Entry{5, "e"}});
  raft::FileStorage again(dir.path(), SyncMode::kNone);
  EXPECT_EQ(again.last_index(), 3u);
  EXPECT_EQ(again.entry(3), (Entry{5, "e"}));
}

// A node restarted from FileStorage rejoins with its log and votes intact.
TEST(RaftFileStorage, ANodeRestartedFromDiskKeepsItsVote) {
  testing::TempDir dir;
  raft::Config config;
  config.id = 1;
  config.peers = {2, 3};
  {
    raft::FileStorage s(dir.path(), SyncMode::kNone);
    raft::RaftNode node(config, &s);
    Message vote;
    vote.type = MessageType::kVoteRequest;
    vote.from = 2;
    vote.to = 1;
    vote.term = 7;
    node.Step(vote);
    const auto out = node.TakeMessages();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].granted);
  }
  raft::FileStorage s(dir.path(), SyncMode::kNone);
  raft::RaftNode node(config, &s);
  Message rival;
  rival.type = MessageType::kVoteRequest;
  rival.from = 3;
  rival.to = 1;
  rival.term = 7;
  node.Step(rival);
  const auto out = node.TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].granted) << "voted twice in term 7 after a restart";
}

}  // namespace
}  // namespace lldstore
