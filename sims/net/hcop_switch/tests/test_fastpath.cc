/*
 * Unit Test: PrimitiveEngine Fast Path Logic
 * Standalone test harness.
 */

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <string>
#include <memory>

#include <hcop/hcop_proto.h>
#include <hcop/paxos_proto.h>
#include <hcop/lock_proto.h>
#include <hcop/barrier_proto.h>

#include "sims/net/hcop_switch/primitive_engine.h"
#include "sims/net/hcop_switch/switch_config.h"

using namespace hcop_switch;
using namespace hcop;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                      \
  do {                                                  \
    ++tests_run;                                        \
    std::printf("  %-50s ", #name);                     \
  } while (0)

#define PASS()                                          \
  do {                                                  \
    ++tests_passed;                                     \
    std::printf("PASS\n");                              \
    return;                                             \
  } while (0)

#define FAIL(msg)                                       \
  do {                                                  \
    std::printf("FAIL: %s\n", msg);                     \
    return;                                             \
  } while (0)

#define ASSERT_EQ(a, b)                                 \
  do {                                                  \
    if ((a) != (b)) {                                   \
        std::printf("FAIL: expected %d == %d\n", (int)(a), (int)(b)); \
        return;                                         \
    }                                                   \
  } while (0)

#define ASSERT_TRUE(expr)                               \
  do {                                                  \
    if (!(expr)) FAIL("expected true: " #expr);         \
  } while (0)

struct PrimitiveEngineHack {
  const SwitchConfig *config_;
  std::unique_ptr<paxos::PaxosNode> paxos_;
  std::unique_ptr<lock::LockManager> locks_;
  std::unique_ptr<barrier::BarrierManager> barriers_;
};

struct TestContext {
  PrimitiveEngine engine;
  SwitchConfig config;
  
  TestContext(size_t sram_pages = 6400,
             SwitchConfig::PlacementMode mode = SwitchConfig::kProcessAndForward) {
    config.sram_pages_total = sram_pages;
    config.node_port_map[0] = 0; 
    config.node_port_map[1] = 1; 
    config.node_port_map[2] = 2; 
    config.switch_node_id = 0;   
    config.num_replicas = 3;     
    config.fallback_port_index = 3;
    config.placement_mode = mode;
    
    engine.Init(config);
  }
  
  void ConfigureBarrier(uint32_t barrier_id, uint16_t n) {
      auto *hack = reinterpret_cast<PrimitiveEngineHack*>(&engine);
      hack->barriers_->SetParticipants(barrier_id, n);
  }
  
  void SeedProposal(uint32_t instance_id, const char *val) {
      auto *hack = reinterpret_cast<PrimitiveEngineHack*>(&engine);
      std::vector<paxos::OutMessage> out;
      hack->paxos_->Propose(instance_id, val, std::strlen(val), out);
  }
};

static void test_paxos_accept() {
    TEST(PaxosAccept_UnicastToProposer);
    TestContext ctx;
    std::vector<uint8_t> pkt(100, 0);
    pkt[12] = (hcop::kHcopEtherType >> 8); 
    pkt[13] = (hcop::kHcopEtherType & 0xFF);
    
    auto *hdr = reinterpret_cast<hcop::HcopHeader*>(pkt.data() + 14);
    hdr->primitive_type = hcop::kPrimitivePaxos;
    hdr->payload_len = sizeof(paxos::AcceptMsg);
    
    auto *msg = reinterpret_cast<paxos::AcceptMsg*>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    msg->hdr.msg_type = paxos::kAccept;
    msg->hdr.sender_id = 1; 
    msg->hdr.num_replicas = 3;
    msg->hdr.instance_id = 0;
    msg->proposal.round = 1;
    msg->proposal.node_id = 1;
    msg->value_len = 4;
    std::memcpy(msg->value, "test", 4);
    
    RoutingDecision d = ctx.engine.HandlePacket(pkt, 1, 0); 
    
    ASSERT_EQ(d.action, RoutingDecision::kUnicast);
    ASSERT_EQ(d.dst_ports.size(), 1u);
    ASSERT_EQ(d.dst_ports[0], 1); 
    
    auto *out_hdr = reinterpret_cast<paxos::PaxosMsgHeader*>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    ASSERT_EQ(out_hdr->msg_type, paxos::kAccepted);
    PASS();
}

static void test_paxos_quorum_learn() {
    TEST(PaxosQuorum_LearnMulticast);
    TestContext ctx;
    
    ctx.SeedProposal(0, "val1");
    
    // 1. Accepted from Node 1
    std::vector<uint8_t> pkt1(100, 0);
    pkt1[12] = (hcop::kHcopEtherType >> 8); 
    pkt1[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr1 = reinterpret_cast<hcop::HcopHeader*>(pkt1.data() + 14);
    hdr1->primitive_type = hcop::kPrimitivePaxos;
    hdr1->payload_len = sizeof(paxos::AcceptedMsg);
    auto *msg1 = reinterpret_cast<paxos::AcceptedMsg*>(pkt1.data() + 14 + sizeof(hcop::HcopHeader));
    msg1->hdr.msg_type = paxos::kAccepted;
    msg1->hdr.sender_id = 1;
    msg1->hdr.num_replicas = 3;
    msg1->hdr.instance_id = 0;
    msg1->proposal.round = 1;
    msg1->proposal.node_id = 0; 
    msg1->value_len = 4;
    std::memcpy(msg1->value, "val1", 4);
    ctx.engine.HandlePacket(pkt1, 1, 0); 
    
    // 2. Accepted from Node 2 (Reaches Quorum 2/3)
    std::vector<uint8_t> pkt2(100, 0);
    pkt2[12] = (hcop::kHcopEtherType >> 8); 
    pkt2[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr2 = reinterpret_cast<hcop::HcopHeader*>(pkt2.data() + 14);
    hdr2->primitive_type = hcop::kPrimitivePaxos;
    hdr2->payload_len = sizeof(paxos::AcceptedMsg);
    auto *msg2 = reinterpret_cast<paxos::AcceptedMsg*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    msg2->hdr.msg_type = paxos::kAccepted;
    msg2->hdr.sender_id = 2; 
    msg2->hdr.num_replicas = 3;
    msg2->hdr.instance_id = 0;
    msg2->proposal.round = 1;
    msg2->proposal.node_id = 0; 
    msg2->value_len = 4;
    std::memcpy(msg2->value, "val1", 4);
    
    RoutingDecision d = ctx.engine.HandlePacket(pkt2, 2, 100);
    
    auto *out_hdr = reinterpret_cast<paxos::PaxosMsgHeader*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    
    if (d.action != RoutingDecision::kMulticast) {
        int dst = (d.dst_ports.empty()) ? -99 : d.dst_ports[0];
        std::printf("DEBUG: Action=%d DestCount=%zu MsgType=%d DstPort=%d\n", 
            (int)d.action, d.dst_ports.size(), (int)out_hdr->msg_type, dst);
    }
    
    ASSERT_EQ(d.action, RoutingDecision::kMulticast);
    ASSERT_EQ(out_hdr->msg_type, paxos::kLearn);
    PASS();
}

static void test_paxos_overflow() {
    TEST(PaxosOverflow_ExceptionToDpu);
    TestContext ctx(0); 
    
    // 1. Instance 0. With total=0 -> capacities=1. Created 1 instance (size 1). OK.
    std::vector<uint8_t> pkt1(100, 0);
    pkt1[12] = (hcop::kHcopEtherType >> 8); 
    pkt1[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr1 = reinterpret_cast<hcop::HcopHeader*>(pkt1.data() + 14);
    hdr1->primitive_type = hcop::kPrimitivePaxos;
    hdr1->payload_len = sizeof(paxos::PrepareMsg);
    auto *msg1 = reinterpret_cast<paxos::PrepareMsg*>(pkt1.data() + 14 + sizeof(hcop::HcopHeader));
    msg1->hdr.msg_type = paxos::kPrepare;
    msg1->hdr.sender_id = 1;
    msg1->hdr.instance_id = 0;
    ctx.engine.HandlePacket(pkt1, 1, 0); 
    
    // 2. Instance 1. Size=1. New instance -> Size 2. > Max 1? 
    // Wait, Init logic: if pages=0 -> capacity=1.
    // If PaxosNode(max=1).
    // Access inst 0. Count 1. 1 >= 1?
    // GetOrCreateInstance: if (instances_.size() >= max_instances_ && ...)
    // If size=1, max=1. 1 >= 1 is true.
    // So NEW instance should fail.
    // pkt1 created ins 0.
    // pkt2 checks.
    
    std::vector<uint8_t> pkt2(100, 0);
    pkt2[12] = (hcop::kHcopEtherType >> 8); 
    pkt2[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr2 = reinterpret_cast<hcop::HcopHeader*>(pkt2.data() + 14);
    hdr2->primitive_type = hcop::kPrimitivePaxos;
    hdr2->payload_len = sizeof(paxos::PrepareMsg);
    auto *msg2 = reinterpret_cast<paxos::PrepareMsg*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    msg2->hdr.msg_type = paxos::kPrepare;
    msg2->hdr.sender_id = 1;
    msg2->hdr.instance_id = 1; 
    
    RoutingDecision d = ctx.engine.HandlePacket(pkt2, 1, 0);
    ASSERT_EQ(d.action, RoutingDecision::kToFallback);
    ASSERT_EQ(hdr2->exception_type, hcop::kPaxosStateOverflow);
    PASS();
}

static void test_lock_acquire_grant() {
    TEST(LockAcquire_GrantUnicast);
    TestContext ctx;
    std::vector<uint8_t> pkt(100, 0);
    pkt[12] = (hcop::kHcopEtherType >> 8); 
    pkt[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr = reinterpret_cast<hcop::HcopHeader*>(pkt.data() + 14);
    hdr->primitive_type = hcop::kPrimitiveLock;
    hdr->payload_len = sizeof(lock::AcquireMsg);
    auto *msg = reinterpret_cast<lock::AcquireMsg*>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    msg->hdr.msg_type = lock::kAcquire;
    msg->hdr.requester_id = 1;
    msg->hdr.lock_key = 123;
    msg->lease_duration_ns = 1000;
    
    RoutingDecision d = ctx.engine.HandlePacket(pkt, 1, 1000); 
    ASSERT_EQ(d.action, RoutingDecision::kUnicast);
    ASSERT_EQ(d.dst_ports[0], 1); 
    auto *out_hdr = reinterpret_cast<lock::LockMsgHeader*>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    ASSERT_EQ(out_hdr->msg_type, lock::kGrant);
    PASS();
}

static void test_lock_contention() {
    TEST(LockContention_ExceptionToDpu);
    TestContext ctx; 
    
    std::vector<uint8_t> pkt1(100, 0);
    pkt1[12] = (hcop::kHcopEtherType >> 8); 
    pkt1[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr1 = reinterpret_cast<hcop::HcopHeader*>(pkt1.data() + 14);
    hdr1->primitive_type = hcop::kPrimitiveLock;
    hdr1->payload_len = sizeof(lock::AcquireMsg);
    auto *msg1 = reinterpret_cast<lock::AcquireMsg*>(pkt1.data() + 14 + sizeof(hcop::HcopHeader));
    msg1->hdr.msg_type = lock::kAcquire;
    msg1->hdr.requester_id = 1;
    msg1->hdr.lock_key = 123;
    msg1->lease_duration_ns = 100000;
    
    RoutingDecision d1 = ctx.engine.HandlePacket(pkt1, 1, 0);
    if (d1.action != RoutingDecision::kUnicast) FAIL("Grant failed");
    
    std::vector<uint8_t> pkt2(100, 0);
    pkt2[12] = (hcop::kHcopEtherType >> 8); 
    pkt2[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr2 = reinterpret_cast<hcop::HcopHeader*>(pkt2.data() + 14);
    hdr2->primitive_type = hcop::kPrimitiveLock;
    hdr2->payload_len = sizeof(lock::AcquireMsg);
    auto *msg2 = reinterpret_cast<lock::AcquireMsg*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    msg2->hdr.msg_type = lock::kAcquire;
    msg2->hdr.requester_id = 2; 
    msg2->hdr.lock_key = 123; 
    
    RoutingDecision d2 = ctx.engine.HandlePacket(pkt2, 2, 100);
    
    ASSERT_EQ(d2.action, RoutingDecision::kToFallback);
    ASSERT_EQ(hdr2->exception_type, hcop::kLockContention);
    PASS();
}

static void test_lock_release() {
    // FissLock fire-and-forget semantics (Decision #6):
    // LockManager::Release() frees the key internally (holder_id → kFree,
    // garbage-collects if no waiters), returns kOk, but produces NO output
    // wire message back to the releaser. The host already knows it released.
    // If waiters existed, Release would auto-grant to the next waiter
    // (unicast GRANT to that waiter — not to the releaser).
    // With max_waiters=0 on the switch fast-path, there are never waiters,
    // so Release always produces kDrop (no packet out).
    TEST(LockRelease_FireAndForget);
    TestContext ctx;
    std::vector<uint8_t> pkt1(100, 0); 
    pkt1[12] = (hcop::kHcopEtherType >> 8); 
    pkt1[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr1 = reinterpret_cast<hcop::HcopHeader*>(pkt1.data() + 14);
    hdr1->primitive_type = hcop::kPrimitiveLock;
    hdr1->payload_len = sizeof(lock::AcquireMsg);
    auto *msg1 = reinterpret_cast<lock::AcquireMsg*>(pkt1.data() + 14 + sizeof(hcop::HcopHeader));
    msg1->hdr.msg_type = lock::kAcquire;
    msg1->hdr.requester_id = 1;
    msg1->hdr.lock_key = 999;
    msg1->lease_duration_ns = 100000;
    ctx.engine.HandlePacket(pkt1, 1, 0);
    
    std::vector<uint8_t> pkt2(100, 0); 
    pkt2[12] = (hcop::kHcopEtherType >> 8); 
    pkt2[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr2 = reinterpret_cast<hcop::HcopHeader*>(pkt2.data() + 14);
    hdr2->primitive_type = hcop::kPrimitiveLock;
    hdr2->payload_len = sizeof(lock::ReleaseMsg);
    auto *msg2 = reinterpret_cast<lock::ReleaseMsg*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    msg2->hdr.msg_type = lock::kRelease;
    msg2->hdr.requester_id = 1;
    msg2->hdr.lock_key = 999;
    
    RoutingDecision d = ctx.engine.HandlePacket(pkt2, 1, 100);
    // Key IS freed (kOk), but no ack wire message → kDrop (no packet out).
    ASSERT_EQ(d.action, RoutingDecision::kDrop);
    PASS();
}

static void test_barrier_logic() {
    TEST(Barrier_AllPaths);
    TestContext ctx;
    ctx.ConfigureBarrier(1, 2);
    
    // 1. Partial
    std::vector<uint8_t> pkt1(100, 0);
    pkt1[12] = (hcop::kHcopEtherType >> 8); 
    pkt1[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr1 = reinterpret_cast<hcop::HcopHeader*>(pkt1.data() + 14);
    hdr1->primitive_type = hcop::kPrimitiveBarrier;
    hdr1->payload_len = sizeof(barrier::ArriveMsg);
    auto *msg1 = reinterpret_cast<barrier::ArriveMsg*>(pkt1.data() + 14 + sizeof(hcop::HcopHeader));
    msg1->hdr.msg_type = barrier::kArrive;
    msg1->hdr.sender_id = 1;
    msg1->hdr.generation = 0;
    msg1->hdr.barrier_id = 1;
    RoutingDecision d1 = ctx.engine.HandlePacket(pkt1, 1, 0);
    ASSERT_EQ(d1.action, RoutingDecision::kDrop); 
    
    // 2. Final -> RELEASE
    std::vector<uint8_t> pkt2(100, 0);
    pkt2[12] = (hcop::kHcopEtherType >> 8); 
    pkt2[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr2 = reinterpret_cast<hcop::HcopHeader*>(pkt2.data() + 14);
    hdr2->primitive_type = hcop::kPrimitiveBarrier;
    hdr2->payload_len = sizeof(barrier::ArriveMsg);
    auto *msg2 = reinterpret_cast<barrier::ArriveMsg*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    msg2->hdr.msg_type = barrier::kArrive;
    msg2->hdr.sender_id = 2; 
    msg2->hdr.generation = 0;
    msg2->hdr.barrier_id = 1;
    RoutingDecision d2 = ctx.engine.HandlePacket(pkt2, 2, 10);
    ASSERT_EQ(d2.action, RoutingDecision::kMulticast); 
    auto *out_hdr = reinterpret_cast<barrier::BarrierMsgHeader*>(pkt2.data() + 14 + sizeof(hcop::HcopHeader));
    ASSERT_EQ(out_hdr->msg_type, barrier::kRelease);
    int gen = out_hdr->generation;
    bool gen_ok = (gen == 0) || (gen == 1);
    ASSERT_TRUE(gen_ok);
    
    // 3. Late Arrival
    std::vector<uint8_t> pkt3(100, 0);
    pkt3[12] = (hcop::kHcopEtherType >> 8); 
    pkt3[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr3 = reinterpret_cast<hcop::HcopHeader*>(pkt3.data() + 14);
    hdr3->primitive_type = hcop::kPrimitiveBarrier;
    hdr3->payload_len = sizeof(barrier::ArriveMsg);
    auto *msg3 = reinterpret_cast<barrier::ArriveMsg*>(pkt3.data() + 14 + sizeof(hcop::HcopHeader));
    msg3->hdr.msg_type = barrier::kArrive;
    msg3->hdr.sender_id = 1;
    msg3->hdr.generation = 0; 
    msg3->hdr.barrier_id = 1;
    
    RoutingDecision d3 = ctx.engine.HandlePacket(pkt3, 1, 20);
    ASSERT_EQ(d3.action, RoutingDecision::kToFallback);
    ASSERT_EQ(hdr3->exception_type, hcop::kBarrierLateArrival);
    
    PASS(); 
}

// =========================================================================
// Placement Mode Tests
// =========================================================================

static std::vector<uint8_t> make_hcop_pkt() {
    std::vector<uint8_t> pkt(100, 0);
    pkt[12] = (hcop::kHcopEtherType >> 8);
    pkt[13] = (hcop::kHcopEtherType & 0xFF);
    auto *hdr = reinterpret_cast<hcop::HcopHeader*>(pkt.data() + 14);
    hdr->primitive_type = hcop::kPrimitivePaxos;
    hdr->payload_len = sizeof(paxos::AcceptMsg);
    auto *msg = reinterpret_cast<paxos::AcceptMsg*>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    msg->hdr.msg_type = paxos::kAccept;
    msg->hdr.sender_id = 1;
    msg->hdr.num_replicas = 3;
    msg->hdr.instance_id = 0;
    msg->proposal.round = 1;
    msg->proposal.node_id = 1;
    msg->value_len = 4;
    std::memcpy(msg->value, "test", 4);
    return pkt;
}

static void test_forward_only_to_fallback() {
    // kForwardOnly: HCOP packet from client port -> forward to fallback port.
    TEST(ForwardOnly_ClientToFallback);
    TestContext ctx(6400, SwitchConfig::kForwardOnly);
    auto pkt = make_hcop_pkt();
    RoutingDecision d = ctx.engine.HandlePacket(pkt, 0, 0); // ingress=client port 0
    ASSERT_EQ(d.action, RoutingDecision::kToFallback);
    PASS();
}

static void test_forward_only_response_flood() {
    // kForwardOnly: HCOP packet from fallback port -> L2 flood to clients.
    TEST(ForwardOnly_FallbackResponseFlood);
    TestContext ctx(6400, SwitchConfig::kForwardOnly);
    auto pkt = make_hcop_pkt();
    RoutingDecision d = ctx.engine.HandlePacket(pkt, 3, 0); // ingress=fallback port 3
    ASSERT_EQ(d.action, RoutingDecision::kMulticast);
    PASS();
}

static void test_process_forward_response_flood() {
    // kProcessAndForward: packet from fallback port -> L2 flood (don't re-process).
    TEST(ProcessForward_FallbackResponseFlood);
    TestContext ctx;
    auto pkt = make_hcop_pkt();
    RoutingDecision d = ctx.engine.HandlePacket(pkt, 3, 0); // ingress=fallback port 3
    ASSERT_EQ(d.action, RoutingDecision::kMulticast);
    PASS();
}

int main() {
    std::printf("=== PrimitiveEngine Comprehensive Tests ===\n");
    test_paxos_accept();
    test_paxos_quorum_learn();
    test_paxos_overflow();
    test_lock_acquire_grant();
    test_lock_contention();
    test_lock_release();
    test_barrier_logic();
    std::printf("\n--- Placement Mode Tests ---\n");
    test_forward_only_to_fallback();
    test_forward_only_response_flood();
    test_process_forward_response_flood();
    std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
