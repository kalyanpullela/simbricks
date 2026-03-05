/*
 * Lock DPU Handler Integration Test
 *
 * Validates the full pipeline: EthRx → DPU processing → LockDpuHandler
 * → LockManager state machine → SendEth response.
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
#include <hcop/lock_proto.h>
#include <hcop/lock_state.h>

#include "../dpu_bm.h"
#include "../lock_dpu_handler.h"

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

static size_t BuildLockFrame(uint8_t *buf, size_t buf_size,
                              uint32_t op_id,
                              const void *lock_payload,
                              size_t lock_len) {
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + lock_len;
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
  hdr->primitive_type = hcop::kPrimitiveLock;
  hdr->exception_type = 0;
  hdr->operation_id = op_id;
  hdr->source_tier = hcop::kTierSwitch;
  hdr->num_tier_crossings = 0;
  hdr->tier_path = 0;
  hdr->payload_len = static_cast<uint16_t>(lock_len);

  // Lock payload
  std::memcpy(buf + 14 + sizeof(hcop::HcopHeader), lock_payload, lock_len);

  return frame_len;
}

// ====================================================================
// Tests
// ====================================================================

static void test_acquire_produces_grant_response() {
  TEST(acquire_msg_produces_grant_response_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<lock::LockDpuHandler>();
  auto *handler_raw = handler.get();
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  lock::AcquireMsg acquire = {};
  acquire.hdr.owner_hint = 255;
  acquire.hdr.msg_type = lock::kAcquire;
  acquire.hdr.requester_id = 2;
  acquire.hdr.lock_key = 100;
  acquire.lease_duration_ns = 5'000'000;

  uint8_t frame[512];
  size_t len = BuildLockFrame(frame, sizeof(frame), 42, &acquire, sizeof(acquire));
  ASSERT_TRUE(len > 0);

  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  ASSERT_EQ(runner.EventCount(), 1u);

  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 1u);

  const auto &resp = sent_frames[0];
  ASSERT_TRUE(resp.size() >= 14 + sizeof(hcop::HcopHeader) + sizeof(lock::LockMsgHeader));

  // Verify HCOP header.
  const hcop::HcopHeader *resp_hcop =
      reinterpret_cast<const hcop::HcopHeader *>(resp.data() + 14);
  ASSERT_EQ(resp_hcop->primitive_type, hcop::kPrimitiveLock);
  ASSERT_EQ(resp_hcop->source_tier, hcop::kTierDpu);

  // Verify lock payload is GRANT.
  const lock::LockMsgHeader *resp_lock =
      reinterpret_cast<const lock::LockMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_lock->msg_type, lock::kGrant);
  ASSERT_EQ(resp_lock->lock_key, 100u);

  // Key should be held.
  const auto *ks = handler_raw->Manager().GetKeyState(100);
  ASSERT_TRUE(ks != nullptr);
  ASSERT_EQ(ks->holder_id, 2u);

  PASS();
}

static void test_acquire_on_held_key_denied() {
  TEST(acquire_on_held_key_produces_deny_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<lock::LockDpuHandler>();
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  // First acquire by node 0.
  lock::AcquireMsg acq1 = {};
  acq1.hdr.owner_hint = 255;
  acq1.hdr.msg_type = lock::kAcquire;
  acq1.hdr.requester_id = 0;
  acq1.hdr.lock_key = 50;
  acq1.lease_duration_ns = 0;

  uint8_t frame[512];
  size_t len = BuildLockFrame(frame, sizeof(frame), 1, &acq1, sizeof(acq1));
  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 1u);  // GRANT

  // Second acquire by node 1 — same key.
  lock::AcquireMsg acq2 = {};
  acq2.hdr.owner_hint = 255;
  acq2.hdr.msg_type = lock::kAcquire;
  acq2.hdr.requester_id = 1;
  acq2.hdr.lock_key = 50;
  acq2.lease_duration_ns = 0;

  len = BuildLockFrame(frame, sizeof(frame), 2, &acq2, sizeof(acq2));
  runner.SetTime(1'000'000);  // 1µs later
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 2u);  // DENY

  const auto &resp = sent_frames[1];
  const lock::LockMsgHeader *resp_lock =
      reinterpret_cast<const lock::LockMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_lock->msg_type, lock::kDeny);

  PASS();
}

static void test_release_grants_next_waiter() {
  TEST(release_auto_grants_next_waiter_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<lock::LockDpuHandler>();
  dev.RegisterHandler(std::move(handler));

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  // Node 0 acquires key 10.
  lock::AcquireMsg acq1 = {};
  acq1.hdr.owner_hint = 255;
  acq1.hdr.msg_type = lock::kAcquire;
  acq1.hdr.requester_id = 0;
  acq1.hdr.lock_key = 10;
  acq1.lease_duration_ns = 0;

  uint8_t frame[512];
  size_t len = BuildLockFrame(frame, sizeof(frame), 1, &acq1, sizeof(acq1));
  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  // Node 1 waits (DENY + queued).
  lock::AcquireMsg acq2 = {};
  acq2.hdr.owner_hint = 255;
  acq2.hdr.msg_type = lock::kAcquire;
  acq2.hdr.requester_id = 1;
  acq2.hdr.lock_key = 10;
  acq2.lease_duration_ns = 0;

  len = BuildLockFrame(frame, sizeof(frame), 2, &acq2, sizeof(acq2));
  runner.SetTime(1'000'000);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  sent_frames.clear();

  // Node 0 releases.
  lock::ReleaseMsg rel = {};
  rel.hdr.owner_hint = 255;
  rel.hdr.msg_type = lock::kRelease;
  rel.hdr.requester_id = 0;
  rel.hdr.lock_key = 10;

  len = BuildLockFrame(frame, sizeof(frame), 3, &rel, sizeof(rel));
  runner.SetTime(2'000'000);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  // Should produce a GRANT to node 1.
  ASSERT_EQ(sent_frames.size(), 1u);

  const auto &resp = sent_frames[0];
  const lock::LockMsgHeader *resp_lock =
      reinterpret_cast<const lock::LockMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_lock->msg_type, lock::kGrant);

  PASS();
}

static void test_core_released_after_lock_processing() {
  TEST(arm_core_released_after_lock_processing);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<lock::LockDpuHandler>();
  dev.RegisterHandler(std::move(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  lock::AcquireMsg acq = {};
  acq.hdr.owner_hint = 255;
  acq.hdr.msg_type = lock::kAcquire;
  acq.hdr.requester_id = 0;
  acq.hdr.lock_key = 1;
  acq.lease_duration_ns = 0;

  uint8_t frame[512];
  size_t len = BuildLockFrame(frame, sizeof(frame), 1, &acq, sizeof(acq));

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
  std::printf("=== Lock DPU Handler Integration Tests ===\n");
  test_acquire_produces_grant_response();
  test_acquire_on_held_key_denied();
  test_release_grants_next_waiter();
  test_core_released_after_lock_processing();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
