/*
 * Unit tests for DramStore.
 */

#include <cstdio>
#include <cstring>

#include "../dpu_bm.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { ++tests_run; std::printf("  %-50s ", #name); } while (0)
#define PASS() do { ++tests_passed; std::printf("PASS\n"); return; } while (0)
#define FAIL(msg) do { std::printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) FAIL("expected " #a " == " #b); } while (0)
#define ASSERT_TRUE(a) do { if (!(a)) FAIL("expected " #a " to be true"); } while (0)
#define ASSERT_FALSE(a) do { if (a) FAIL("expected " #a " to be false"); } while (0)
#define ASSERT_NULL(a) do { if ((a) != nullptr) FAIL("expected " #a " == nullptr"); } while (0)
#define ASSERT_NOT_NULL(a) do { if ((a) == nullptr) FAIL("expected " #a " != nullptr"); } while (0)

static void test_allocate_and_read() {
  TEST(allocate_and_read);
  dpu::DramStore store(1024);
  ASSERT_TRUE(store.Allocate(1, 64));
  ASSERT_EQ(store.UsedBytes(), 64u);

  size_t len = 0;
  const uint8_t *data = store.Read(1, &len);
  ASSERT_NOT_NULL(data);
  ASSERT_EQ(len, 64u);
  PASS();
}

static void test_write_and_read_back() {
  TEST(write_and_read_back);
  dpu::DramStore store(1024);
  store.Allocate(42, 4);

  uint8_t wdata[] = {0xDE, 0xAD, 0xBE, 0xEF};
  ASSERT_TRUE(store.Write(42, wdata, 4));

  size_t len = 0;
  const uint8_t *rdata = store.Read(42, &len);
  ASSERT_EQ(len, 4u);
  ASSERT_EQ(std::memcmp(rdata, wdata, 4), 0);
  PASS();
}

static void test_capacity_overflow() {
  TEST(capacity_overflow_rejected);
  dpu::DramStore store(100);
  ASSERT_TRUE(store.Allocate(1, 60));
  ASSERT_TRUE(store.Allocate(2, 40));
  // Should fail — would exceed 100 bytes.
  ASSERT_FALSE(store.Allocate(3, 1));
  ASSERT_EQ(store.UsedBytes(), 100u);
  PASS();
}

static void test_duplicate_key_rejected() {
  TEST(duplicate_key_rejected);
  dpu::DramStore store(1024);
  ASSERT_TRUE(store.Allocate(1, 10));
  ASSERT_FALSE(store.Allocate(1, 10));
  ASSERT_EQ(store.UsedBytes(), 10u);
  PASS();
}

static void test_free_and_reuse() {
  TEST(free_and_reuse);
  dpu::DramStore store(100);
  ASSERT_TRUE(store.Allocate(1, 100));
  ASSERT_EQ(store.UsedBytes(), 100u);

  store.Free(1);
  ASSERT_EQ(store.UsedBytes(), 0u);

  // Can allocate again.
  ASSERT_TRUE(store.Allocate(2, 50));
  ASSERT_EQ(store.UsedBytes(), 50u);
  PASS();
}

static void test_free_nonexistent_is_safe() {
  TEST(free_nonexistent_is_noop);
  dpu::DramStore store(1024);
  store.Free(999);  // should not crash
  ASSERT_EQ(store.UsedBytes(), 0u);
  PASS();
}

static void test_read_nonexistent_key() {
  TEST(read_nonexistent_returns_null);
  dpu::DramStore store(1024);
  size_t len = 42;
  const uint8_t *data = store.Read(999, &len);
  ASSERT_NULL(data);
  ASSERT_EQ(len, 0u);
  PASS();
}

static void test_write_nonexistent_key() {
  TEST(write_nonexistent_returns_false);
  dpu::DramStore store(1024);
  uint8_t buf[4] = {};
  ASSERT_FALSE(store.Write(999, buf, 4));
  PASS();
}

static void test_capacity_accessor() {
  TEST(capacity_accessor);
  dpu::DramStore store(8192);
  ASSERT_EQ(store.CapacityBytes(), 8192u);
  PASS();
}

int main() {
  std::printf("=== DRAM Store Tests ===\n");
  test_allocate_and_read();
  test_write_and_read_back();
  test_capacity_overflow();
  test_duplicate_key_rejected();
  test_free_and_reuse();
  test_free_nonexistent_is_safe();
  test_read_nonexistent_key();
  test_write_nonexistent_key();
  test_capacity_accessor();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
