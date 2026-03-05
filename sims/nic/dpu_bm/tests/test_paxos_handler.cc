/*
 * Paxos DPU Handler Integration Test
 *
 * Validates the full pipeline: EthRx → DPU processing → PaxosDpuHandler
 * → PaxosNode state machine → SendEth response.
 *
 * Uses the TestRunner harness from the pipeline smoke test.
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include <simbricks/nicbm/nicbm.h>

extern "C" {
#include <simbricks/parser/parser.h>
}

#include <hcop/hcop_proto.h>
#include <hcop/paxos_proto.h>
#include <hcop/paxos_state.h>

#include "../dpu_bm.h"
#include "../paxos_dpu_handler.h"

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

// ---- TestRunner (same as pipeline test) ----

class TestRunner : public nicbm::Runner {
 public:
  explicit TestRunner(Device &dev) : Runner(dev) {
    pcieAdapterParams_ = static_cast<SimbricksAdapterParams *>(
        calloc(1, sizeof(SimbricksAdapterParams)));
    netAdapterParams_ = static_cast<SimbricksAdapterParams *>(
        calloc(1, sizeof(SimbricksAdapterParams)));
  }

  void SetTime(uint64_t t) { main_time_ = t; }
  size_t EventCount() const { return events_.size(); }

  void FireNextEvent() {
    if (events_.empty()) return;
    auto it = events_.begin();
    nicbm::TimedEvent *evt = *it;
    main_time_ = evt->time_;
    events_.erase(it);
    dev_.Timed(*evt);
  }
};

// ---- Frame builder ----

static size_t BuildPaxosFrame(uint8_t *buf, size_t buf_size,
                               uint32_t op_id,
                               const void *paxos_payload,
                               size_t paxos_len) {
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + paxos_len;
  if (frame_len > buf_size) return 0;

  std::memset(buf, 0, frame_len);

  // Ethernet header
  buf[0] = 0x00; buf[1] = 0x11; buf[2] = 0x22;
  buf[3] = 0x33; buf[4] = 0x44; buf[5] = 0x55;
  buf[6] = 0x00; buf[7] = 0xAA; buf[8] = 0xBB;
  buf[9] = 0xCC; buf[10] = 0xDD; buf[11] = 0xEE;
  buf[12] = (hcop::kHcopEtherType >> 8) & 0xFF;
  buf[13] = hcop::kHcopEtherType & 0xFF;

  // HCOP header
  hcop::HcopHeader *hdr = reinterpret_cast<hcop::HcopHeader *>(buf + 14);
  hdr->primitive_type = hcop::kPrimitivePaxos;
  hdr->exception_type = 0;
  hdr->operation_id = op_id;
  hdr->source_tier = hcop::kTierSwitch;
  hdr->num_tier_crossings = 0;
  hdr->tier_path = 0;
  hdr->payload_len = static_cast<uint16_t>(paxos_len);

  // Paxos payload
  std::memcpy(buf + 14 + sizeof(hcop::HcopHeader), paxos_payload, paxos_len);

  return frame_len;
}

// ====================================================================
// Tests
// ====================================================================

static void test_accept_produces_accepted_response() {
  TEST(accept_msg_produces_accepted_response_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<paxos::PaxosDpuHandler>(1, 3);
  auto *handler_raw = handler.get();
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  paxos::AcceptMsg accept = {};
  accept.hdr.msg_type = paxos::kAccept;
  accept.hdr.sender_id = 0;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {1, 0, 0};
  accept.value_len = 5;
  std::memcpy(accept.value, "hello", 5);

  uint8_t frame[512];
  size_t len = BuildPaxosFrame(frame, sizeof(frame), 42, &accept, sizeof(accept));
  ASSERT_TRUE(len > 0);

  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  ASSERT_EQ(runner.EventCount(), 1u);

  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 1u);

  const auto &resp = sent_frames[0];
  ASSERT_TRUE(resp.size() >= 14 + sizeof(hcop::HcopHeader) + sizeof(paxos::PaxosMsgHeader));

  const hcop::HcopHeader *resp_hcop =
      reinterpret_cast<const hcop::HcopHeader *>(resp.data() + 14);
  ASSERT_EQ(resp_hcop->primitive_type, hcop::kPrimitivePaxos);
  ASSERT_EQ(resp_hcop->source_tier, hcop::kTierDpu);

  const paxos::PaxosMsgHeader *resp_paxos =
      reinterpret_cast<const paxos::PaxosMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_paxos->msg_type, paxos::kAccepted);
  ASSERT_EQ(resp_paxos->sender_id, 1u);
  ASSERT_EQ(resp_paxos->instance_id, 1u);

  const auto *inst = handler_raw->Node().GetInstance(1);
  ASSERT_TRUE(inst != nullptr);
  ASSERT_EQ(inst->accepted_value_len, 5u);

  PASS();
}

static void test_prepare_produces_promise_response() {
  TEST(prepare_msg_produces_promise_response_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<paxos::PaxosDpuHandler>(1, 3);
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  paxos::PrepareMsg prepare = {};
  prepare.hdr.msg_type = paxos::kPrepare;
  prepare.hdr.sender_id = 0;
  prepare.hdr.num_replicas = 3;
  prepare.hdr.instance_id = 5;
  prepare.proposal = {10, 0, 0};

  uint8_t frame[512];
  size_t len = BuildPaxosFrame(frame, sizeof(frame), 100, &prepare, sizeof(prepare));

  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 1u);

  const auto &resp = sent_frames[0];
  const paxos::PaxosMsgHeader *resp_paxos =
      reinterpret_cast<const paxos::PaxosMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_paxos->msg_type, paxos::kPromise);
  ASSERT_EQ(resp_paxos->sender_id, 1u);
  ASSERT_EQ(resp_paxos->instance_id, 5u);

  PASS();
}

static void test_nack_on_stale_accept() {
  TEST(nack_on_accept_with_stale_proposal_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<paxos::PaxosDpuHandler>(1, 3);
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  paxos::PrepareMsg prepare = {};
  prepare.hdr.msg_type = paxos::kPrepare;
  prepare.hdr.sender_id = 0;
  prepare.hdr.num_replicas = 3;
  prepare.hdr.instance_id = 1;
  prepare.proposal = {100, 0, 0};

  uint8_t frame[512];
  size_t len = BuildPaxosFrame(frame, sizeof(frame), 1, &prepare, sizeof(prepare));
  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();
  ASSERT_EQ(sent_frames.size(), 1u);

  paxos::AcceptMsg accept = {};
  accept.hdr.msg_type = paxos::kAccept;
  accept.hdr.sender_id = 2;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {5, 2, 0};
  accept.value_len = 3;
  std::memcpy(accept.value, "abc", 3);

  len = BuildPaxosFrame(frame, sizeof(frame), 2, &accept, sizeof(accept));
  runner.SetTime(1000000);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 2u);

  const auto &resp = sent_frames[1];
  const paxos::PaxosMsgHeader *resp_paxos =
      reinterpret_cast<const paxos::PaxosMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_paxos->msg_type, paxos::kNack);

  PASS();
}

static void test_core_released_after_paxos_processing() {
  TEST(arm_core_released_after_paxos_processing);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<paxos::PaxosDpuHandler>(0, 3);
  dev.RegisterHandler(std::move(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  paxos::AcceptMsg accept = {};
  accept.hdr.msg_type = paxos::kAccept;
  accept.hdr.sender_id = 1;
  accept.hdr.num_replicas = 3;
  accept.hdr.instance_id = 1;
  accept.proposal = {1, 1, 0};
  accept.value_len = 4;
  std::memcpy(accept.value, "test", 4);

  uint8_t frame[512];
  size_t len = BuildPaxosFrame(frame, sizeof(frame), 1, &accept, sizeof(accept));

  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  ASSERT_EQ(dev.CorePool().ActiveCount(), 1u);

  runner.FireNextEvent();
  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  PASS();
}

// ====================================================================
// Main
// ====================================================================

int main() {
  std::printf("=== Paxos DPU Handler Integration Tests ===\n");
  test_accept_produces_accepted_response();
  test_prepare_produces_promise_response();
  test_nack_on_stale_accept();
  test_core_released_after_paxos_processing();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
