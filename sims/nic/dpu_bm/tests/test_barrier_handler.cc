/*
 * Barrier DPU Handler Integration Test
 *
 * Validates the full pipeline: EthRx → DPU processing → BarrierDpuHandler
 * → BarrierManager state machine → SendEth response.
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
#include <hcop/barrier_proto.h>
#include <hcop/barrier_state.h>

#include "../dpu_bm.h"
#include "../barrier_dpu_handler.h"

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

// ---- TestRunner (same as other tests) ----

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

static size_t BuildBarrierFrame(uint8_t *buf, size_t buf_size,
                                uint32_t op_id,
                                const void *payload,
                                size_t len) {
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + len;
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
  hdr->primitive_type = hcop::kPrimitiveBarrier;
  hdr->exception_type = 0;
  hdr->operation_id = op_id;
  hdr->source_tier = hcop::kTierSwitch;
  hdr->num_tier_crossings = 0;
  hdr->tier_path = 0;
  hdr->payload_len = static_cast<uint16_t>(len);

  // Payload
  std::memcpy(buf + 14 + sizeof(hcop::HcopHeader), payload, len);

  return frame_len;
}

// ====================================================================
// Tests
// ====================================================================

static void test_barrier_release_broadcast() {
  TEST(barrier_release_broadcast_via_dpu);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<barrier::BarrierDpuHandler>();
  auto *handler_raw = handler.get();
  dev.RegisterHandler(std::move(handler));

  // Configure barrier N=3
  handler_raw->Manager().SetParticipants(10, 3);

  std::vector<std::vector<uint8_t>> sent_frames;
  dev.SetEthSendCallback([&](const void *data, size_t len) {
    sent_frames.emplace_back(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  });

  uint8_t frame[512];
  barrier::ArriveMsg arr = {};
  arr.hdr.msg_type = barrier::kArrive;
  arr.hdr.barrier_id = 10;
  arr.hdr.generation = 0;

  // Arrival 1
  arr.hdr.sender_id = 0;
  size_t len = BuildBarrierFrame(frame, sizeof(frame), 1, &arr, sizeof(arr));
  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();
  ASSERT_EQ(sent_frames.size(), 0u);

  // Arrival 2
  arr.hdr.sender_id = 1;
  len = BuildBarrierFrame(frame, sizeof(frame), 2, &arr, sizeof(arr));
  runner.SetTime(100);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();
  ASSERT_EQ(sent_frames.size(), 0u);

  // Arrival 3 -> Release!
  arr.hdr.sender_id = 2;
  len = BuildBarrierFrame(frame, sizeof(frame), 3, &arr, sizeof(arr));
  runner.SetTime(200);
  dev.EthRx(0, frame, len);
  runner.FireNextEvent();

  ASSERT_EQ(sent_frames.size(), 1u); // Broadcast RELEASE

  const auto &resp = sent_frames[0];
  const barrier::BarrierMsgHeader *resp_hdr =
      reinterpret_cast<const barrier::BarrierMsgHeader *>(
          resp.data() + 14 + sizeof(hcop::HcopHeader));
  ASSERT_EQ(resp_hdr->msg_type, barrier::kRelease);
  ASSERT_EQ(resp_hdr->generation, 0u);
  
  // Verify broadcast MAC (ff:ff:ff:ff:ff:ff)
  for(int i=0; i<6; ++i) ASSERT_EQ(resp[i], 0xFF);

  PASS();
}

static void test_core_lifecycle() {
  TEST(core_lifecycle_barrier);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto handler = std::make_unique<barrier::BarrierDpuHandler>();
  dev.RegisterHandler(std::move(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  uint8_t frame[512];
  barrier::ArriveMsg arr = {};
  arr.hdr.msg_type = barrier::kArrive;
  arr.hdr.barrier_id = 1;
  size_t len = BuildBarrierFrame(frame, sizeof(frame), 1, &arr, sizeof(arr));

  runner.SetTime(0);
  dev.EthRx(0, frame, len);
  ASSERT_EQ(dev.CorePool().ActiveCount(), 1u);

  runner.FireNextEvent();
  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  PASS();
}

int main() {
  std::printf("=== Barrier DPU Handler Tests ===\n");
  test_barrier_release_broadcast();
  test_core_lifecycle();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
