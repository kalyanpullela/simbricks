/*
 * Unit tests for DpuConfig (JSON loading and validation).
 * Standalone test — no gtest, matches SimBricks' existing test pattern.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "../dpu_config.h"

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
    if ((a) != (b)) FAIL("expected " #a " == " #b);    \
  } while (0)

#define ASSERT_THROWS(expr)                             \
  do {                                                  \
    bool threw = false;                                 \
    try { expr; } catch (...) { threw = true; }         \
    if (!threw) FAIL("expected exception from " #expr); \
  } while (0)

// Helper: write a string to a temp file, return its path.
static std::string WriteTmpJson(const std::string &content) {
  static int seq = 0;
  std::string path = "/tmp/dpu_test_config_" + std::to_string(seq++) + ".json";
  std::ofstream ofs(path);
  ofs << content;
  ofs.close();
  return path;
}

// ----- Tests -----

static void test_default_config() {
  TEST(default_config);
  dpu::DpuConfig cfg = dpu::DefaultConfig();
  ASSERT_EQ(cfg.arm_cores, 16u);
  ASSERT_EQ(cfg.dram_capacity_mb, 16384u);
  ASSERT_EQ(cfg.per_packet_base_latency_ns, 2000u);
  ASSERT_EQ(cfg.service_rate_pps, 40'000'000u);
  ASSERT_EQ(cfg.host_processing_latency_ns, 15000u);
  ASSERT_EQ(cfg.crypto_accel, true);
  ASSERT_EQ(cfg.doca_flow_steering, true);
  PASS();
}

static void test_load_full_json() {
  TEST(load_full_json);
  std::string json = R"({
    "arm_cores": 8,
    "dram_capacity_mb": 8192,
    "pcie_gen": 4,
    "pcie_lanes": 8,
    "per_packet_base_latency_ns": 3000,
    "service_rate_pps": 20000000,
    "host_processing_latency_ns": 10000,
    "crypto_accel": false,
    "doca_flow_steering": false
  })";
  auto path = WriteTmpJson(json);
  dpu::DpuConfig cfg = dpu::LoadConfig(path);
  ASSERT_EQ(cfg.arm_cores, 8u);
  ASSERT_EQ(cfg.dram_capacity_mb, 8192u);
  ASSERT_EQ(cfg.pcie_gen, 4u);
  ASSERT_EQ(cfg.pcie_lanes, 8u);
  ASSERT_EQ(cfg.per_packet_base_latency_ns, 3000u);
  ASSERT_EQ(cfg.service_rate_pps, 20000000u);
  ASSERT_EQ(cfg.host_processing_latency_ns, 10000u);
  ASSERT_EQ(cfg.crypto_accel, false);
  ASSERT_EQ(cfg.doca_flow_steering, false);
  PASS();
}

static void test_load_partial_json() {
  TEST(load_partial_json_uses_defaults);
  std::string json = R"({ "arm_cores": 4 })";
  auto path = WriteTmpJson(json);
  dpu::DpuConfig cfg = dpu::LoadConfig(path);
  ASSERT_EQ(cfg.arm_cores, 4u);
  // All other fields should be default.
  ASSERT_EQ(cfg.dram_capacity_mb, 16384u);
  ASSERT_EQ(cfg.per_packet_base_latency_ns, 2000u);
  PASS();
}

static void test_empty_json_object() {
  TEST(empty_json_object_uses_all_defaults);
  auto path = WriteTmpJson("{}");
  dpu::DpuConfig cfg = dpu::LoadConfig(path);
  ASSERT_EQ(cfg.arm_cores, 16u);
  PASS();
}

static void test_reject_zero_arm_cores() {
  TEST(reject_zero_arm_cores);
  auto path = WriteTmpJson(R"({ "arm_cores": 0 })");
  ASSERT_THROWS(dpu::LoadConfig(path));
  PASS();
}

static void test_reject_zero_dram() {
  TEST(reject_zero_dram);
  auto path = WriteTmpJson(R"({ "dram_capacity_mb": 0 })");
  ASSERT_THROWS(dpu::LoadConfig(path));
  PASS();
}

static void test_reject_zero_latency() {
  TEST(reject_zero_latency);
  auto path = WriteTmpJson(R"({ "per_packet_base_latency_ns": 0 })");
  ASSERT_THROWS(dpu::LoadConfig(path));
  PASS();
}

static void test_reject_zero_rate() {
  TEST(reject_zero_rate);
  auto path = WriteTmpJson(R"({ "service_rate_pps": 0 })");
  ASSERT_THROWS(dpu::LoadConfig(path));
  PASS();
}

static void test_reject_malformed_json() {
  TEST(reject_malformed_json);
  auto path = WriteTmpJson("{ not valid json }}}");
  ASSERT_THROWS(dpu::LoadConfig(path));
  PASS();
}

static void test_reject_nonexistent_file() {
  TEST(reject_nonexistent_file);
  ASSERT_THROWS(dpu::LoadConfig("/tmp/does_not_exist_dpu_config.json"));
  PASS();
}

int main() {
  std::printf("=== DPU Config Tests ===\n");
  test_default_config();
  test_load_full_json();
  test_load_partial_json();
  test_empty_json_object();
  test_reject_zero_arm_cores();
  test_reject_zero_dram();
  test_reject_zero_latency();
  test_reject_zero_rate();
  test_reject_malformed_json();
  test_reject_nonexistent_file();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
