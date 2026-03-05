/*
 * Unit tests for ArmCorePool.
 */

#include <cstdio>

#include "../dpu_bm.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { ++tests_run; std::printf("  %-50s ", #name); } while (0)
#define PASS() do { ++tests_passed; std::printf("PASS\n"); return; } while (0)
#define FAIL(msg) do { std::printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) FAIL("expected " #a " == " #b); } while (0)
#define ASSERT_TRUE(a) do { if (!(a)) FAIL("expected " #a " to be true"); } while (0)
#define ASSERT_FALSE(a) do { if (a) FAIL("expected " #a " to be false"); } while (0)

static void test_acquire_single() {
  TEST(acquire_single_core);
  dpu::ArmCorePool pool(4);
  auto c = pool.TryAcquire();
  ASSERT_TRUE(c.has_value());
  ASSERT_EQ(pool.ActiveCount(), 1u);
  PASS();
}

static void test_acquire_to_capacity() {
  TEST(acquire_to_capacity);
  dpu::ArmCorePool pool(4);
  for (int i = 0; i < 4; ++i) {
    auto c = pool.TryAcquire();
    ASSERT_TRUE(c.has_value());
  }
  ASSERT_EQ(pool.ActiveCount(), 4u);
  PASS();
}

static void test_acquire_beyond_capacity() {
  TEST(acquire_beyond_capacity_returns_nullopt);
  dpu::ArmCorePool pool(2);
  pool.TryAcquire();
  pool.TryAcquire();
  auto c = pool.TryAcquire();
  ASSERT_FALSE(c.has_value());
  ASSERT_EQ(pool.ActiveCount(), 2u);
  PASS();
}

static void test_release_and_reacquire() {
  TEST(release_and_reacquire);
  dpu::ArmCorePool pool(2);
  auto c0 = pool.TryAcquire();
  auto c1 = pool.TryAcquire();
  (void)c1;
  ASSERT_EQ(pool.ActiveCount(), 2u);

  pool.Release(c0.value());
  ASSERT_EQ(pool.ActiveCount(), 1u);

  auto c2 = pool.TryAcquire();
  ASSERT_TRUE(c2.has_value());
  ASSERT_EQ(pool.ActiveCount(), 2u);
  PASS();
}

static void test_capacity_accessor() {
  TEST(capacity_accessor);
  dpu::ArmCorePool pool(16);
  ASSERT_EQ(pool.Capacity(), 16u);
  ASSERT_EQ(pool.ActiveCount(), 0u);
  PASS();
}

static void test_unique_core_ids() {
  TEST(unique_core_ids);
  dpu::ArmCorePool pool(4);
  uint32_t ids[4];
  for (int i = 0; i < 4; ++i) {
    ids[i] = pool.TryAcquire().value();
  }
  // All IDs should be unique.
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      if (ids[i] == ids[j]) FAIL("duplicate core IDs");
    }
  }
  PASS();
}

int main() {
  std::printf("=== ARM Core Pool Tests ===\n");
  test_acquire_single();
  test_acquire_to_capacity();
  test_acquire_beyond_capacity();
  test_release_and_reacquire();
  test_capacity_accessor();
  test_unique_core_ids();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
