/*
 * Pipeline Smoke Test — validates the full DPU processing pipeline:
 *
 *   EthRx → parse HCOP header → acquire ARM core → schedule event
 *   → Timed fires → HandlePacket called → SendEth response → release core
 *
 * Uses a TestRunner subclass to drive the event loop without SimBricks
 * shared memory infrastructure.
 */

#include <cstdio>
#include <cstring>

#include <simbricks/nicbm/nicbm.h>

extern "C" {
#include <simbricks/parser/parser.h>
}

#include "../dpu_bm.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { ++tests_run; std::printf("  %-55s ", #name); } while (0)
#define PASS() do { ++tests_passed; std::printf("PASS\n"); return; } while (0)
#define FAIL(msg) do { std::printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) FAIL("expected " #a " == " #b); } while (0)
#define ASSERT_TRUE(a) do { if (!(a)) FAIL("expected " #a " to be true"); } while (0)

// ======================================================================
// TestRunner: exposes Runner internals for standalone testing
// ======================================================================

class TestRunner : public nicbm::Runner {
 public:
  explicit TestRunner(Device &dev) : Runner(dev) {
    // Runner::~Runner() calls SimbricksParametersFree() on these without
    // null-checking. Allocate minimal dummy objects so free() works safely.
    pcieAdapterParams_ = static_cast<SimbricksAdapterParams *>(
        calloc(1, sizeof(SimbricksAdapterParams)));
    netAdapterParams_ = static_cast<SimbricksAdapterParams *>(
        calloc(1, sizeof(SimbricksAdapterParams)));
  }

  void SetTime(uint64_t t) { main_time_ = t; }
  uint64_t GetTime() const { return main_time_; }
  size_t EventCount() const { return events_.size(); }

  /** Get the scheduled time of the next event (0 if none). */
  uint64_t NextEventTime() const {
    if (events_.empty()) return 0;
    return (*events_.begin())->time_;
  }

  /**
   * Advance time to the next event and fire it.
   * This calls dev_.Timed(*evt), which in DpuDevice will:
   *   - call the handler
   *   - release the ARM core
   *   - delete the event
   */
  void FireNextEvent() {
    if (events_.empty()) return;
    auto it = events_.begin();
    nicbm::TimedEvent *evt = *it;
    main_time_ = evt->time_;
    events_.erase(it);
    dev_.Timed(*evt);
  }
};

// ======================================================================
// DummyHandler: records whether HandlePacket was called
// ======================================================================

class DummyHandler : public dpu::PrimitiveHandler {
 public:
  bool was_called = false;
  uint32_t last_op_id = 0;
  size_t last_payload_len = 0;

  // State for response
  bool send_response = true;

  uint16_t PrimitiveType() const override {
    return hcop::kPrimitivePaxos;  // type 1
  }

  void HandlePacket(dpu::DpuDevice &dev, const void *data, size_t len,
                    dpu::PacketContext &ctx) override {
    was_called = true;
    last_op_id = ctx.operation_id;
    last_payload_len = len;

    if (send_response) {
      // Build a minimal response frame:
      // Ethernet header (14) + HcopHeader (12)
      uint8_t resp[14 + sizeof(hcop::HcopHeader)];
      std::memset(resp, 0, sizeof(resp));

      // Ethernet: swap src/dst MAC, set EtherType
      const uint8_t *in_frame = static_cast<const uint8_t *>(ctx.full_frame);
      std::memcpy(resp, in_frame + 6, 6);      // dst = original src
      std::memcpy(resp + 6, in_frame, 6);       // src = original dst
      resp[12] = (hcop::kHcopEtherType >> 8) & 0xFF;
      resp[13] = hcop::kHcopEtherType & 0xFF;

      // HCOP header
      hcop::HcopHeader *rhdr =
          reinterpret_cast<hcop::HcopHeader *>(resp + 14);
      rhdr->primitive_type = hcop::kPrimitivePaxos;
      rhdr->exception_type = hcop::kPaxosNoException;
      rhdr->operation_id = ctx.operation_id;
      rhdr->source_tier = hcop::kTierDpu;
      rhdr->num_tier_crossings = 0;
      rhdr->tier_path = 0;
      rhdr->payload_len = 0;

      dev.SendEth(resp, sizeof(resp));
    }
  }
};

// ======================================================================
// Helper: build a minimal HCOP Ethernet frame
// ======================================================================

static size_t BuildHcopFrame(uint8_t *buf, size_t buf_size,
                             uint32_t operation_id,
                             uint16_t primitive_type = hcop::kPrimitivePaxos,
                             size_t payload_len = 0) {
  size_t frame_len = 14 + sizeof(hcop::HcopHeader) + payload_len;
  if (frame_len > buf_size) return 0;

  std::memset(buf, 0, frame_len);

  // Ethernet header
  // dst MAC: 00:11:22:33:44:55
  buf[0] = 0x00; buf[1] = 0x11; buf[2] = 0x22;
  buf[3] = 0x33; buf[4] = 0x44; buf[5] = 0x55;
  // src MAC: 00:AA:BB:CC:DD:EE
  buf[6] = 0x00; buf[7] = 0xAA; buf[8] = 0xBB;
  buf[9] = 0xCC; buf[10] = 0xDD; buf[11] = 0xEE;
  // EtherType: HCOP
  buf[12] = (hcop::kHcopEtherType >> 8) & 0xFF;
  buf[13] = hcop::kHcopEtherType & 0xFF;

  // HCOP header
  hcop::HcopHeader *hdr =
      reinterpret_cast<hcop::HcopHeader *>(buf + 14);
  hdr->primitive_type = primitive_type;
  hdr->exception_type = 0;
  hdr->operation_id = operation_id;
  hdr->source_tier = hcop::kTierSwitch;
  hdr->num_tier_crossings = 0;
  hdr->tier_path = 0;
  hdr->payload_len = static_cast<uint16_t>(payload_len);

  return frame_len;
}

// ======================================================================
// Tests
// ======================================================================

static void test_handler_called() {
  TEST(handler_process_method_called);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  // Register dummy handler and suppress EthSend (no shared mem).
  auto *handler = new DummyHandler();
  handler->send_response = false;
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  // Build and inject a packet.
  uint8_t frame[128];
  size_t len = BuildHcopFrame(frame, sizeof(frame), /*op_id=*/42);
  ASSERT_TRUE(len > 0);

  runner.SetTime(1000000);  // 1 µs
  dev.EthRx(0, frame, len);

  // Event should be scheduled.
  ASSERT_EQ(runner.EventCount(), 1u);
  ASSERT_TRUE(!handler->was_called);

  // Fire the event.
  runner.FireNextEvent();

  ASSERT_TRUE(handler->was_called);
  ASSERT_EQ(handler->last_op_id, 42u);
  PASS();
}

static void test_core_acquire_and_release() {
  TEST(arm_core_acquired_and_released);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto *handler = new DummyHandler();
  handler->send_response = false;
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  // Inject packet.
  uint8_t frame[128];
  size_t len = BuildHcopFrame(frame, sizeof(frame), /*op_id=*/1);
  runner.SetTime(0);
  dev.EthRx(0, frame, len);

  // Core should be acquired (1 active).
  ASSERT_EQ(dev.CorePool().ActiveCount(), 1u);

  // Fire event — core should be released.
  runner.FireNextEvent();
  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);

  PASS();
}

static void test_processing_delay() {
  TEST(processing_delay_matches_config);

  dpu::DpuConfig cfg = dpu::DefaultConfig();
  // Use a very specific latency to validate.
  cfg.per_packet_base_latency_ns = 5000;  // 5 µs
  dpu::DpuDevice dev(cfg);
  TestRunner runner(dev);

  auto *handler = new DummyHandler();
  handler->send_response = false;
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  // Inject packet at time = 10,000,000 ps (10 µs).
  uint64_t inject_time = 10'000'000;  // ps
  runner.SetTime(inject_time);

  uint8_t frame[128];
  size_t len = BuildHcopFrame(frame, sizeof(frame), /*op_id=*/7);
  dev.EthRx(0, frame, len);

  // Event should be scheduled at inject_time + 5000 ns * 1000 ps/ns.
  uint64_t expected_time = inject_time + 5000ULL * 1000ULL;  // 15,000,000 ps
  ASSERT_EQ(runner.NextEventTime(), expected_time);

  // Fire it — time should advance to expected_time.
  runner.FireNextEvent();
  ASSERT_EQ(runner.GetTime(), expected_time);
  ASSERT_TRUE(handler->was_called);

  PASS();
}

static void test_response_via_eth_send() {
  TEST(response_packet_exits_via_eth_send);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto *handler = new DummyHandler();
  handler->send_response = true;
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));

  // Capture EthSend output.
  bool eth_send_called = false;
  size_t sent_len = 0;
  std::vector<uint8_t> sent_data;

  dev.SetEthSendCallback([&](const void *data, size_t len) {
    eth_send_called = true;
    sent_len = len;
    sent_data.assign(static_cast<const uint8_t *>(data),
                     static_cast<const uint8_t *>(data) + len);
  });

  // Inject packet.
  uint8_t frame[128];
  size_t len = BuildHcopFrame(frame, sizeof(frame), /*op_id=*/99);
  runner.SetTime(0);
  dev.EthRx(0, frame, len);

  ASSERT_TRUE(!eth_send_called);

  // Fire event — handler should send response.
  runner.FireNextEvent();

  ASSERT_TRUE(eth_send_called);
  ASSERT_TRUE(sent_len >= hcop::kMinHcopFrameLen);

  // Verify response HCOP header.
  const hcop::HcopHeader *resp_hdr =
      reinterpret_cast<const hcop::HcopHeader *>(sent_data.data() + 14);
  ASSERT_EQ(resp_hdr->primitive_type, hcop::kPrimitivePaxos);
  ASSERT_EQ(resp_hdr->operation_id, 99u);
  ASSERT_EQ(resp_hdr->source_tier, hcop::kTierDpu);
  ASSERT_EQ(resp_hdr->exception_type,
            static_cast<uint16_t>(hcop::kPaxosNoException));

  PASS();
}

static void test_drop_unknown_primitive() {
  TEST(unknown_primitive_dropped_gracefully);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  // Register handler for Paxos only.
  auto *handler = new DummyHandler();
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  // Send a LOCK packet (type 2) — no handler registered.
  uint8_t frame[128];
  size_t len = BuildHcopFrame(frame, sizeof(frame), /*op_id=*/1,
                              /*primitive_type=*/99);
  runner.SetTime(0);
  dev.EthRx(0, frame, len);

  // Should be dropped — no event scheduled, no core acquired.
  ASSERT_EQ(runner.EventCount(), 0u);
  ASSERT_EQ(dev.CorePool().ActiveCount(), 0u);
  ASSERT_TRUE(!handler->was_called);

  PASS();
}

static void test_drop_non_hcop_ethertype() {
  TEST(non_hcop_ethertype_dropped);

  dpu::DpuDevice dev;
  TestRunner runner(dev);

  auto *handler = new DummyHandler();
  dev.RegisterHandler(std::unique_ptr<dpu::PrimitiveHandler>(handler));
  dev.SetEthSendCallback([](const void *, size_t) {});

  // Build a frame with regular IPv4 EtherType (0x0800).
  uint8_t frame[64];
  std::memset(frame, 0, sizeof(frame));
  frame[12] = 0x08;
  frame[13] = 0x00;

  runner.SetTime(0);
  dev.EthRx(0, frame, sizeof(frame));

  ASSERT_EQ(runner.EventCount(), 0u);
  ASSERT_TRUE(!handler->was_called);

  PASS();
}

static void test_drop_runt_frame() {
  TEST(runt_frame_dropped);

  dpu::DpuDevice dev;
  TestRunner runner(dev);
  dev.SetEthSendCallback([](const void *, size_t) {});

  // Frame too short to contain Ethernet + HCOP headers.
  uint8_t frame[10];
  std::memset(frame, 0, sizeof(frame));

  runner.SetTime(0);
  dev.EthRx(0, frame, sizeof(frame));

  ASSERT_EQ(runner.EventCount(), 0u);

  PASS();
}

// ======================================================================
// Main
// ======================================================================

int main() {
  std::printf("=== DPU Pipeline Smoke Tests ===\n");
  test_handler_called();
  test_core_acquire_and_release();
  test_processing_delay();
  test_response_via_eth_send();
  test_drop_unknown_primitive();
  test_drop_non_hcop_ethertype();
  test_drop_runt_frame();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
