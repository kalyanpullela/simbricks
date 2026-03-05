/*
 * Paxos State Machine Unit Tests
 *
 * Tests the PaxosNode state machine in isolation (no SimBricks deps):
 * 1. Stable-leader propose → accept → commit
 * 2. Full leader election (Phase 1 + Phase 2)
 * 3. Conflicting proposals and NACK handling
 * 4. STATE_OVERFLOW exception (via status return)
 * 5. Learn notification propagation
 * 6. Already-committed instance handling
 *
 * Location: lib/hcop/tests/ — no SimBricks dependencies.
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "../hcop_proto.h"
#include "../paxos_state.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { ++tests_run; std::printf("  %-60s ", #name); } while (0)
#define PASS() do { ++tests_passed; std::printf("PASS\n"); return; } while (0)
#define FAIL(msg) do { std::printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { \
  char _buf[256]; std::snprintf(_buf, sizeof(_buf), \
    "expected " #a "==%s but got %llu vs %llu", #b, \
    (unsigned long long)(a), (unsigned long long)(b)); \
  FAIL(_buf); } } while (0)
#define ASSERT_TRUE(a) do { if (!(a)) FAIL("expected " #a " to be true"); } while (0)
#define ASSERT_FALSE(a) do { if ((a)) FAIL("expected " #a " to be false"); } while (0)
#define ASSERT_STATUS(s, expected) do { if ((s) != (expected)) { \
  FAIL("unexpected PaxosStatus"); } } while (0)

using namespace paxos;

// ====================================================================
// Tests
// ====================================================================

static void test_proposal_num_comparison() {
  TEST(proposal_num_lexicographic_comparison);

  ProposalNum a = {1, 0, 0};
  ProposalNum b = {2, 0, 0};
  ProposalNum c = {1, 1, 0};

  ASSERT_TRUE(a < b);
  ASSERT_TRUE(a < c);   // same round, higher node_id
  ASSERT_TRUE(b > c);   // higher round wins
  ASSERT_TRUE(a == a);
  ASSERT_TRUE(a != b);
  ASSERT_FALSE(a.IsNull());

  ProposalNum null = {0, 0, 0};
  ASSERT_TRUE(null.IsNull());

  PASS();
}

static void test_node_construction() {
  TEST(node_construction_with_3_replicas);

  PaxosNode node(0, 3);
  ASSERT_EQ(node.NodeId(), 0u);
  ASSERT_EQ(node.NumReplicas(), 3u);
  ASSERT_EQ(node.Quorum(), 2u);
  ASSERT_TRUE(node.IsLeader());  // node 0 is default leader
  ASSERT_EQ(node.InstanceCount(), 0u);
  ASSERT_EQ(node.MaxInstances(), 10000u);

  PaxosNode node5(2, 5, /*max_instances=*/500);
  ASSERT_EQ(node5.Quorum(), 3u);
  ASSERT_FALSE(node5.IsLeader());
  ASSERT_EQ(node5.MaxInstances(), 500u);

  PASS();
}

static void test_stable_leader_propose_commit() {
  TEST(stable_leader_propose_and_commit_3_replicas);

  PaxosNode leader(0, 3);
  PaxosNode follower1(1, 3);
  PaxosNode follower2(2, 3);

  std::vector<OutMessage> out;
  const char *value = "hello";

  // Step 1: Leader proposes (Phase 2a — skip Phase 1 for stable leader).
  auto status = leader.Propose(/*instance_id=*/1, value, std::strlen(value), out);
  ASSERT_STATUS(status, PaxosStatus::kOk);
  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].dest_id, kBroadcast);

  // Verify it's an Accept message.
  const auto *accept = reinterpret_cast<const AcceptMsg *>(out[0].data.data());
  ASSERT_EQ(accept->hdr.msg_type, kAccept);
  ASSERT_EQ(accept->hdr.sender_id, 0u);
  ASSERT_EQ(accept->hdr.instance_id, 1u);
  ASSERT_EQ(accept->value_len, 5u);

  // Step 2: Deliver Accept to followers (and leader itself as acceptor).
  std::vector<OutMessage> resp_leader, resp_f1, resp_f2;
  leader.HandleMessage(out[0].data.data(), out[0].data.size(), resp_leader);
  follower1.HandleMessage(out[0].data.data(), out[0].data.size(), resp_f1);
  follower2.HandleMessage(out[0].data.data(), out[0].data.size(), resp_f2);

  ASSERT_EQ(resp_leader.size(), 1u);  // Accepted
  ASSERT_EQ(resp_f1.size(), 1u);      // Accepted
  ASSERT_EQ(resp_f2.size(), 1u);      // Accepted

  // Step 3: Deliver Accepted to leader.
  std::vector<OutMessage> commit_out;
  leader.HandleMessage(resp_leader[0].data.data(),
                       resp_leader[0].data.size(), commit_out);

  // First Accepted — not yet quorum.
  ASSERT_EQ(commit_out.size(), 0u);

  leader.HandleMessage(resp_f1[0].data.data(),
                       resp_f1[0].data.size(), commit_out);

  // Second Accepted — quorum! Should broadcast Learn.
  // Learn to nodes 1 and 2 (not self = 0).
  ASSERT_EQ(commit_out.size(), 2u);

  // Verify Learn messages.
  for (const auto &msg : commit_out) {
    const auto *learn = reinterpret_cast<const LearnMsg *>(msg.data.data());
    ASSERT_EQ(learn->hdr.msg_type, kLearn);
    ASSERT_EQ(learn->hdr.instance_id, 1u);
    ASSERT_EQ(learn->value_len, 5u);
    ASSERT_TRUE(std::memcmp(learn->value, "hello", 5) == 0);
  }

  // Step 4: Verify committed state on leader.
  const auto *inst = leader.GetInstance(1);
  ASSERT_TRUE(inst != nullptr);
  ASSERT_TRUE(inst->is_committed);

  // Step 5: Deliver Learn to followers.
  std::vector<OutMessage> learn_out;
  follower1.HandleMessage(commit_out[0].data.data(),
                          commit_out[0].data.size(), learn_out);
  const auto *f1_inst = follower1.GetInstance(1);
  ASSERT_TRUE(f1_inst != nullptr);
  ASSERT_TRUE(f1_inst->is_committed);
  ASSERT_EQ(f1_inst->committed_value_len, 5u);

  PASS();
}

static void test_leader_election() {
  TEST(leader_election_phase1_then_phase2);

  PaxosNode node0(0, 3);
  PaxosNode node1(1, 3);
  PaxosNode node2(2, 3);

  // Node 1 initiates leader election (non-leader).
  std::vector<OutMessage> out;
  auto status = node1.StartLeaderElection(/*instance_id=*/1, out);
  ASSERT_STATUS(status, PaxosStatus::kOk);

  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].dest_id, kBroadcast);
  const auto *prepare = reinterpret_cast<const PrepareMsg *>(out[0].data.data());
  ASSERT_EQ(prepare->hdr.msg_type, kPrepare);
  ASSERT_EQ(prepare->hdr.sender_id, 1u);

  // Deliver Prepare to all nodes.
  std::vector<OutMessage> resp0, resp1, resp2;
  node0.HandleMessage(out[0].data.data(), out[0].data.size(), resp0);
  node1.HandleMessage(out[0].data.data(), out[0].data.size(), resp1);
  node2.HandleMessage(out[0].data.data(), out[0].data.size(), resp2);

  // All should Promise (no prior higher promises).
  ASSERT_EQ(resp0.size(), 1u);
  ASSERT_EQ(resp1.size(), 1u);
  ASSERT_EQ(resp2.size(), 1u);

  const auto *promise0 = reinterpret_cast<const PromiseMsg *>(resp0[0].data.data());
  ASSERT_EQ(promise0->hdr.msg_type, kPromise);

  // Deliver Promises to node1 (the proposer).
  std::vector<OutMessage> phase2_out;
  node1.HandleMessage(resp0[0].data.data(), resp0[0].data.size(), phase2_out);
  ASSERT_EQ(phase2_out.size(), 0u);  // First promise — not yet quorum.

  node1.HandleMessage(resp1[0].data.data(), resp1[0].data.size(), phase2_out);
  ASSERT_EQ(phase2_out.size(), 1u);  // Second promise — quorum! Accept broadcast.

  const auto *accept = reinterpret_cast<const AcceptMsg *>(phase2_out[0].data.data());
  ASSERT_EQ(accept->hdr.msg_type, kAccept);

  PASS();
}

static void test_nack_on_higher_promise() {
  TEST(nack_when_acceptor_has_higher_promise);

  PaxosNode node1(1, 3);

  // Promise round 5.
  PrepareMsg high_prepare = {};
  high_prepare.hdr.msg_type = kPrepare;
  high_prepare.hdr.sender_id = 0;
  high_prepare.hdr.num_replicas = 3;
  high_prepare.hdr.instance_id = 1;
  high_prepare.proposal = {5, 0, 0};

  std::vector<OutMessage> resp;
  node1.HandleMessage(&high_prepare, sizeof(high_prepare), resp);
  ASSERT_EQ(resp.size(), 1u);

  const auto *promise = reinterpret_cast<const PromiseMsg *>(resp[0].data.data());
  ASSERT_EQ(promise->hdr.msg_type, kPromise);

  // Lower proposal (round 2) should be NACKed.
  PrepareMsg low_prepare = {};
  low_prepare.hdr.msg_type = kPrepare;
  low_prepare.hdr.sender_id = 0;
  low_prepare.hdr.num_replicas = 3;
  low_prepare.hdr.instance_id = 1;
  low_prepare.proposal = {2, 0, 0};

  resp.clear();
  node1.HandleMessage(&low_prepare, sizeof(low_prepare), resp);
  ASSERT_EQ(resp.size(), 1u);

  const auto *nack = reinterpret_cast<const NackMsg *>(resp[0].data.data());
  ASSERT_EQ(nack->hdr.msg_type, kNack);
  ASSERT_EQ(nack->highest_seen.round, 5u);

  PASS();
}

static void test_accept_nack_on_higher_promise() {
  TEST(accept_rejected_when_higher_promise_exists);

  PaxosNode node(1, 3);

  // Promise a high proposal.
  PrepareMsg prepare = {};
  prepare.hdr.msg_type = kPrepare;
  prepare.hdr.sender_id = 0;
  prepare.hdr.num_replicas = 3;
  prepare.hdr.instance_id = 1;
  prepare.proposal = {10, 0, 0};

  std::vector<OutMessage> resp;
  node.HandleMessage(&prepare, sizeof(prepare), resp);
  ASSERT_EQ(resp.size(), 1u);  // Promise

  // Accept with lower proposal — should be NACKed.
  AcceptMsg accept = {};
  accept.hdr.msg_type = kAccept;
  accept.hdr.sender_id = 2;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {5, 2, 0};
  accept.value_len = 3;
  std::memcpy(accept.value, "abc", 3);

  resp.clear();
  node.HandleMessage(&accept, sizeof(accept), resp);
  ASSERT_EQ(resp.size(), 1u);

  const auto *nack = reinterpret_cast<const NackMsg *>(resp[0].data.data());
  ASSERT_EQ(nack->hdr.msg_type, kNack);

  PASS();
}

static void test_state_overflow_via_status() {
  TEST(state_overflow_returns_kInstanceOverflow_status);

  PaxosNode node(0, 3, /*max_instances=*/3);

  bool overflow_fired = false;
  node.SetExceptionCallback([&](uint16_t type, uint32_t /*inst*/) {
    if (type == hcop::kPaxosStateOverflow) overflow_fired = true;
  });

  std::vector<OutMessage> out;

  // Fill 3 instances.
  ASSERT_STATUS(node.Propose(1, "a", 1, out), PaxosStatus::kOk);
  ASSERT_STATUS(node.Propose(2, "b", 1, out), PaxosStatus::kOk);
  ASSERT_STATUS(node.Propose(3, "c", 1, out), PaxosStatus::kOk);
  ASSERT_FALSE(overflow_fired);
  ASSERT_EQ(node.InstanceCount(), 3u);

  // 4th instance should return kInstanceOverflow.
  out.clear();
  auto status = node.Propose(4, "d", 1, out);
  ASSERT_STATUS(status, PaxosStatus::kInstanceOverflow);
  ASSERT_TRUE(overflow_fired);
  ASSERT_EQ(out.size(), 0u);

  PASS();
}

static void test_overflow_on_accept_handler() {
  TEST(accept_from_network_returns_overflow_when_log_full);

  PaxosNode node(1, 3, /*max_instances=*/1);

  std::vector<OutMessage> out;

  // Fill the single slot.
  AcceptMsg accept1 = {};
  accept1.hdr.msg_type = kAccept;
  accept1.hdr.sender_id = 0;
  accept1.hdr.num_replicas = 3;
  accept1.hdr.instance_id = 1;
  accept1.proposal = {1, 0, 0};
  accept1.value_len = 1;
  accept1.value[0] = 'a';

  auto s = node.HandleMessage(&accept1, sizeof(accept1), out);
  ASSERT_STATUS(s, PaxosStatus::kOk);
  ASSERT_EQ(node.InstanceCount(), 1u);

  // Second instance should overflow.
  AcceptMsg accept2 = {};
  accept2.hdr.msg_type = kAccept;
  accept2.hdr.sender_id = 0;
  accept2.hdr.num_replicas = 3;
  accept2.hdr.instance_id = 2;
  accept2.proposal = {2, 0, 0};
  accept2.value_len = 1;
  accept2.value[0] = 'b';

  out.clear();
  s = node.HandleMessage(&accept2, sizeof(accept2), out);
  ASSERT_STATUS(s, PaxosStatus::kInstanceOverflow);
  ASSERT_EQ(out.size(), 0u);  // no response produced

  PASS();
}

static void test_leader_election_exception() {
  TEST(leader_election_fires_exception_callback);

  PaxosNode node(1, 3);
  bool election_fired = false;
  node.SetExceptionCallback([&](uint16_t type, uint32_t /*inst*/) {
    if (type == hcop::kPaxosLeaderElection) election_fired = true;
  });

  std::vector<OutMessage> out;
  node.StartLeaderElection(1, out);
  ASSERT_TRUE(election_fired);

  PASS();
}

static void test_duplicate_propose_committed() {
  TEST(propose_rejected_for_committed_instance);

  PaxosNode leader(0, 3);
  PaxosNode f1(1, 3);
  PaxosNode f2(2, 3);

  std::vector<OutMessage> out;

  // Commit instance 1.
  leader.Propose(1, "first", 5, out);
  ASSERT_EQ(out.size(), 1u);

  std::vector<OutMessage> r0, r1, r2;
  leader.HandleMessage(out[0].data.data(), out[0].data.size(), r0);
  f1.HandleMessage(out[0].data.data(), out[0].data.size(), r1);
  f2.HandleMessage(out[0].data.data(), out[0].data.size(), r2);

  std::vector<OutMessage> commit;
  leader.HandleMessage(r0[0].data.data(), r0[0].data.size(), commit);
  leader.HandleMessage(r1[0].data.data(), r1[0].data.size(), commit);
  ASSERT_TRUE(commit.size() > 0);

  // Propose again on same instance.
  out.clear();
  auto status = leader.Propose(1, "second", 6, out);
  ASSERT_STATUS(status, PaxosStatus::kAlreadyCommitted);
  ASSERT_EQ(out.size(), 0u);

  PASS();
}

static void test_accept_on_committed_returns_learn() {
  TEST(accept_on_committed_instance_returns_learn);

  PaxosNode node(0, 3);

  // Manually commit instance 1 via Learn.
  LearnMsg learn = {};
  learn.hdr.msg_type = kLearn;
  learn.hdr.sender_id = 1;
  learn.hdr.num_replicas = 3;
  learn.hdr.instance_id = 1;
  learn.value_len = 5;
  std::memcpy(learn.value, "hello", 5);

  std::vector<OutMessage> out;
  node.HandleMessage(&learn, sizeof(learn), out);

  const auto *inst = node.GetInstance(1);
  ASSERT_TRUE(inst != nullptr);
  ASSERT_TRUE(inst->is_committed);

  // Now send Accept for the same instance.
  AcceptMsg accept = {};
  accept.hdr.msg_type = kAccept;
  accept.hdr.sender_id = 2;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {99, 2, 0};
  accept.value_len = 5;
  std::memcpy(accept.value, "world", 5);

  out.clear();
  auto status = node.HandleMessage(&accept, sizeof(accept), out);
  ASSERT_STATUS(status, PaxosStatus::kAlreadyCommitted);

  // Should respond with Learn (committed value), not Accepted.
  ASSERT_EQ(out.size(), 1u);
  const auto *learn_resp = reinterpret_cast<const LearnMsg *>(out[0].data.data());
  ASSERT_EQ(learn_resp->hdr.msg_type, kLearn);
  ASSERT_EQ(learn_resp->value_len, 5u);
  ASSERT_TRUE(std::memcmp(learn_resp->value, "hello", 5) == 0);

  PASS();
}

static void test_multiple_instances() {
  TEST(multiple_independent_instances);

  PaxosNode node(0, 3);

  std::vector<OutMessage> out;
  node.Propose(1, "first", 5, out);
  node.Propose(2, "second", 6, out);
  node.Propose(3, "third", 5, out);

  ASSERT_EQ(node.InstanceCount(), 3u);
  ASSERT_EQ(out.size(), 3u);

  // Verify each Accept has correct instance_id.
  for (size_t i = 0; i < 3; ++i) {
    const auto *accept = reinterpret_cast<const AcceptMsg *>(out[i].data.data());
    ASSERT_EQ(accept->hdr.instance_id, static_cast<uint32_t>(i + 1));
  }

  PASS();
}

static void test_promise_with_accepted_value() {
  TEST(promise_carries_previously_accepted_value);

  PaxosNode node(1, 3);

  // Accept a value first.
  AcceptMsg accept = {};
  accept.hdr.msg_type = kAccept;
  accept.hdr.sender_id = 0;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {5, 0, 0};
  accept.value_len = 4;
  std::memcpy(accept.value, "prev", 4);

  std::vector<OutMessage> out;
  node.HandleMessage(&accept, sizeof(accept), out);
  ASSERT_EQ(out.size(), 1u);  // Accepted

  // Higher Prepare.
  PrepareMsg prepare = {};
  prepare.hdr.msg_type = kPrepare;
  prepare.hdr.sender_id = 2;
  prepare.hdr.num_replicas = 3;
  prepare.hdr.instance_id = 1;
  prepare.proposal = {10, 2, 0};

  out.clear();
  node.HandleMessage(&prepare, sizeof(prepare), out);
  ASSERT_EQ(out.size(), 1u);

  const auto *promise = reinterpret_cast<const PromiseMsg *>(out[0].data.data());
  ASSERT_EQ(promise->hdr.msg_type, kPromise);
  ASSERT_EQ(promise->accepted_proposal.round, 5u);
  ASSERT_EQ(promise->accepted_value_len, 4u);
  ASSERT_TRUE(std::memcmp(promise->accepted_value, "prev", 4) == 0);

  PASS();
}

static void test_max_instances_constructor() {
  TEST(max_instances_set_via_constructor);

  // Small limit — simulates switch SRAM budget.
  PaxosNode switch_node(0, 3, /*max_instances=*/2);
  ASSERT_EQ(switch_node.MaxInstances(), 2u);

  std::vector<OutMessage> out;
  ASSERT_STATUS(switch_node.Propose(1, "a", 1, out), PaxosStatus::kOk);
  ASSERT_STATUS(switch_node.Propose(2, "b", 1, out), PaxosStatus::kOk);
  ASSERT_STATUS(switch_node.Propose(3, "c", 1, out), PaxosStatus::kInstanceOverflow);

  // Large limit — simulates DPU DRAM budget.
  PaxosNode dpu_node(0, 3, /*max_instances=*/1000000);
  ASSERT_EQ(dpu_node.MaxInstances(), 1000000u);

  PASS();
}

// ====================================================================
// Main
// ====================================================================

int main() {
  std::printf("=== Paxos State Machine Tests ===\n");
  test_proposal_num_comparison();
  test_node_construction();
  test_stable_leader_propose_commit();
  test_leader_election();
  test_nack_on_higher_promise();
  test_accept_nack_on_higher_promise();
  test_state_overflow_via_status();
  test_overflow_on_accept_handler();
  test_leader_election_exception();
  test_duplicate_propose_committed();
  test_accept_on_committed_returns_learn();
  test_multiple_instances();
  test_promise_with_accepted_value();
  test_max_instances_constructor();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
