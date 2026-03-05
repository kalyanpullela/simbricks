/*
 * Lock State Machine Unit Tests
 *
 * Tests the LockManager state machine in isolation (no SimBricks deps):
 * 1. Acquire free key → GRANT
 * 2. Acquire held key → DENY + queued
 * 3. Release → next waiter auto-granted
 * 4. Release by non-holder → kNotHeld
 * 5. Idempotent re-acquire by holder (lease refresh)
 * 6. Lease timeout → auto-release + TIMEOUT sent
 * 7. Timeout with waiters → auto-grant next
 * 8. Key overflow → kKeyOverflow status
 * 9. max_waiters_per_key=0 → immediate CONTENTION
 * 10. Contention queue depth limit
 * 11. Multiple independent keys
 * 12. Release free key → kNotHeld
 * 13. max_keys constructor parameter
 * 14. Garbage collection of released keys
 *
 * Location: lib/hcop/tests/ — no SimBricks dependencies.
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "../hcop_proto.h"
#include "../lock_state.h"

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
  FAIL("unexpected LockStatus"); } } while (0)

using namespace lock;

// ====================================================================
// Tests
// ====================================================================

static void test_acquire_free_key() {
  TEST(acquire_free_key_returns_granted);

  LockManager mgr;
  std::vector<OutMessage> out;

  auto s = mgr.Acquire(/*key=*/100, /*requester=*/0, /*lease=*/0, /*now=*/1000, out);
  ASSERT_STATUS(s, LockStatus::kGranted);
  ASSERT_EQ(out.size(), 1u);

  // Verify GRANT message.
  const auto *grant = reinterpret_cast<const GrantMsg *>(out[0].data.data());
  ASSERT_EQ(grant->hdr.msg_type, kGrant);
  ASSERT_EQ(grant->hdr.lock_key, 100u);
  ASSERT_EQ(out[0].dest_id, 0u);
  // Default lease is 10ms = 10,000,000 ns.
  ASSERT_EQ(grant->lease_expiry_ns, 1000u + 10'000'000u);

  // Key state should show holder.
  const auto *ks = mgr.GetKeyState(100);
  ASSERT_TRUE(ks != nullptr);
  ASSERT_EQ(ks->holder_id, 0u);

  PASS();
}

static void test_acquire_held_key_denied_and_queued() {
  TEST(acquire_held_key_denied_and_queued);

  LockManager mgr;
  std::vector<OutMessage> out;

  // Node 0 acquires key 1.
  mgr.Acquire(1, 0, 0, 1000, out);
  ASSERT_STATUS(mgr.Acquire(1, 0, 0, 1000, out), LockStatus::kGranted);
  out.clear();

  // Node 1 tries to acquire — should be denied but queued.
  auto s = mgr.Acquire(1, 1, 0, 2000, out);
  ASSERT_STATUS(s, LockStatus::kDenied);
  ASSERT_EQ(out.size(), 1u);

  const auto *deny = reinterpret_cast<const DenyMsg *>(out[0].data.data());
  ASSERT_EQ(deny->hdr.msg_type, kDeny);
  ASSERT_EQ(deny->holder_id, 0u);  // held by node 0

  // Verify waiter is queued.
  const auto *ks = mgr.GetKeyState(1);
  ASSERT_EQ(ks->waiters.size(), 1u);

  PASS();
}

static void test_release_grants_next_waiter() {
  TEST(release_auto_grants_next_waiter);

  LockManager mgr;
  std::vector<OutMessage> out;

  // Node 0 acquires key 1.
  mgr.Acquire(1, 0, 5'000'000, 1000, out);
  out.clear();

  // Node 1 and node 2 queue up.
  mgr.Acquire(1, 1, 5'000'000, 2000, out);
  mgr.Acquire(1, 2, 5'000'000, 3000, out);
  out.clear();

  // Node 0 releases.
  auto s = mgr.Release(1, 0, 4000, out);
  ASSERT_STATUS(s, LockStatus::kOk);

  // Should auto-grant to node 1 (FIFO).
  ASSERT_EQ(out.size(), 1u);
  const auto *grant = reinterpret_cast<const GrantMsg *>(out[0].data.data());
  ASSERT_EQ(grant->hdr.msg_type, kGrant);
  ASSERT_EQ(out[0].dest_id, 1u);

  // Key should now be held by node 1.
  const auto *ks = mgr.GetKeyState(1);
  ASSERT_EQ(ks->holder_id, 1u);
  ASSERT_EQ(ks->waiters.size(), 1u);  // node 2 still waiting

  PASS();
}

static void test_release_by_non_holder() {
  TEST(release_by_non_holder_returns_not_held);

  LockManager mgr;
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 0, 1000, out);
  out.clear();

  auto s = mgr.Release(1, /*requester=*/1, 2000, out);
  ASSERT_STATUS(s, LockStatus::kNotHeld);
  ASSERT_EQ(out.size(), 0u);

  // Key should still be held by node 0.
  const auto *ks = mgr.GetKeyState(1);
  ASSERT_EQ(ks->holder_id, 0u);

  PASS();
}

static void test_idempotent_reacquire() {
  TEST(idempotent_reacquire_by_holder_refreshes_lease);

  LockManager mgr;
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 5'000'000, 1000, out);
  out.clear();

  // Same requester acquires again — should refresh lease.
  auto s = mgr.Acquire(1, 0, 8'000'000, 3000, out);
  ASSERT_STATUS(s, LockStatus::kGranted);
  ASSERT_EQ(out.size(), 1u);

  const auto *grant = reinterpret_cast<const GrantMsg *>(out[0].data.data());
  ASSERT_EQ(grant->lease_expiry_ns, 3000u + 8'000'000u);

  PASS();
}

static void test_lease_timeout() {
  TEST(lease_timeout_auto_releases_and_notifies);

  LockManager mgr;
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 10'000, /*now=*/0, out);  // 10µs lease
  out.clear();

  // Check timeouts before expiry — nothing should happen.
  uint32_t expired = mgr.CheckTimeouts(5'000, out);
  ASSERT_EQ(expired, 0u);
  ASSERT_EQ(out.size(), 0u);

  // Check timeouts after expiry.
  expired = mgr.CheckTimeouts(15'000, out);
  ASSERT_EQ(expired, 1u);
  ASSERT_EQ(out.size(), 1u);

  const auto *timeout = reinterpret_cast<const TimeoutMsg *>(out[0].data.data());
  ASSERT_EQ(timeout->hdr.msg_type, kTimeout);
  ASSERT_EQ(out[0].dest_id, 0u);  // sent to the expired holder

  // Key should be freed (and garbage-collected since no waiters).
  ASSERT_TRUE(mgr.GetKeyState(1) == nullptr);

  PASS();
}

static void test_timeout_with_waiters() {
  TEST(timeout_with_waiters_auto_grants_next);

  LockManager mgr;
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 10'000, /*now=*/0, out);  // 10µs lease
  mgr.Acquire(1, 1, 50'000, /*now=*/5000, out);  // node 1 waits
  out.clear();

  // Expire the lease.
  uint32_t expired = mgr.CheckTimeouts(15'000, out);
  ASSERT_EQ(expired, 1u);

  // Should produce: TIMEOUT to node 0 + GRANT to node 1.
  ASSERT_EQ(out.size(), 2u);

  const auto *timeout = reinterpret_cast<const TimeoutMsg *>(out[0].data.data());
  ASSERT_EQ(timeout->hdr.msg_type, kTimeout);
  ASSERT_EQ(out[0].dest_id, 0u);

  const auto *grant = reinterpret_cast<const GrantMsg *>(out[1].data.data());
  ASSERT_EQ(grant->hdr.msg_type, kGrant);
  ASSERT_EQ(out[1].dest_id, 1u);

  // Now held by node 1.
  const auto *ks = mgr.GetKeyState(1);
  ASSERT_EQ(ks->holder_id, 1u);

  PASS();
}

static void test_key_overflow() {
  TEST(key_overflow_returns_kKeyOverflow_status);

  LockManager mgr(/*max_keys=*/2);
  std::vector<OutMessage> out;

  ASSERT_STATUS(mgr.Acquire(1, 0, 0, 0, out), LockStatus::kGranted);
  ASSERT_STATUS(mgr.Acquire(2, 1, 0, 0, out), LockStatus::kGranted);

  out.clear();
  auto s = mgr.Acquire(3, 2, 0, 0, out);
  ASSERT_STATUS(s, LockStatus::kKeyOverflow);
  ASSERT_EQ(out.size(), 0u);
  ASSERT_EQ(mgr.KeyCount(), 2u);

  PASS();
}

static void test_no_queuing_mode() {
  TEST(max_waiters_0_immediate_contention_exception);

  // Switch model: max_waiters_per_key=0 means no queuing at all.
  LockManager mgr(/*max_keys=*/100, /*default_lease=*/10'000'000,
                   /*max_waiters=*/0);

  bool contention_fired = false;
  mgr.SetExceptionCallback([&](uint16_t type, uint64_t /*key*/) {
    if (type == hcop::kLockContention) contention_fired = true;
  });

  std::vector<OutMessage> out;
  mgr.Acquire(1, 0, 0, 0, out);
  out.clear();

  // Second acquire → immediate CONTENTION, no queuing.
  auto s = mgr.Acquire(1, 1, 0, 0, out);
  ASSERT_STATUS(s, LockStatus::kContention);
  ASSERT_TRUE(contention_fired);
  ASSERT_EQ(out.size(), 1u);  // DENY sent

  const auto *deny = reinterpret_cast<const DenyMsg *>(out[0].data.data());
  ASSERT_EQ(deny->hdr.msg_type, kDeny);

  // Verify no waiter was queued.
  const auto *ks = mgr.GetKeyState(1);
  ASSERT_EQ(ks->waiters.size(), 0u);

  PASS();
}

static void test_contention_queue_full() {
  TEST(contention_queue_full_returns_contention);

  LockManager mgr(/*max_keys=*/100, /*default_lease=*/10'000'000,
                   /*max_waiters=*/2);
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 0, 0, out);  // holder
  mgr.Acquire(1, 1, 0, 0, out);  // waiter 1
  mgr.Acquire(1, 2, 0, 0, out);  // waiter 2
  out.clear();

  // Third waiter exceeds max_waiters_per_key=2.
  auto s = mgr.Acquire(1, 3, 0, 0, out);
  ASSERT_STATUS(s, LockStatus::kContention);

  PASS();
}

static void test_multiple_independent_keys() {
  TEST(multiple_independent_keys);

  LockManager mgr;
  std::vector<OutMessage> out;

  ASSERT_STATUS(mgr.Acquire(100, 0, 0, 0, out), LockStatus::kGranted);
  ASSERT_STATUS(mgr.Acquire(200, 1, 0, 0, out), LockStatus::kGranted);
  ASSERT_STATUS(mgr.Acquire(300, 2, 0, 0, out), LockStatus::kGranted);

  ASSERT_EQ(mgr.KeyCount(), 3u);

  // Each key independently held.
  ASSERT_EQ(mgr.GetKeyState(100)->holder_id, 0u);
  ASSERT_EQ(mgr.GetKeyState(200)->holder_id, 1u);
  ASSERT_EQ(mgr.GetKeyState(300)->holder_id, 2u);

  PASS();
}

static void test_release_unknown_key() {
  TEST(release_unknown_key_returns_not_held);

  LockManager mgr;
  std::vector<OutMessage> out;

  auto s = mgr.Release(999, 0, 0, out);
  ASSERT_STATUS(s, LockStatus::kNotHeld);
  ASSERT_EQ(out.size(), 0u);

  PASS();
}

static void test_garbage_collection() {
  TEST(freed_key_with_no_waiters_is_garbage_collected);

  LockManager mgr;
  std::vector<OutMessage> out;

  mgr.Acquire(1, 0, 0, 0, out);
  ASSERT_EQ(mgr.KeyCount(), 1u);

  mgr.Release(1, 0, 1000, out);
  ASSERT_EQ(mgr.KeyCount(), 0u);  // removed after release

  PASS();
}

static void test_handle_message_acquire() {
  TEST(handle_message_acquire_wire_format);

  LockManager mgr;
  std::vector<OutMessage> out;

  AcquireMsg msg = {};
  msg.hdr.msg_type = kAcquire;
  msg.hdr.requester_id = 3;
  msg.hdr.lock_key = 42;
  msg.lease_duration_ns = 5'000'000;

  auto s = mgr.HandleMessage(&msg, sizeof(msg), /*now=*/1000, out);
  ASSERT_STATUS(s, LockStatus::kGranted);
  ASSERT_EQ(out.size(), 1u);

  const auto *grant = reinterpret_cast<const GrantMsg *>(out[0].data.data());
  ASSERT_EQ(grant->hdr.msg_type, kGrant);
  ASSERT_EQ(out[0].dest_id, 3u);

  PASS();
}

static void test_handle_message_release() {
  TEST(handle_message_release_wire_format);

  LockManager mgr;
  std::vector<OutMessage> out;

  // Acquire first.
  mgr.Acquire(42, 3, 0, 0, out);
  out.clear();

  // Release via wire format.
  ReleaseMsg msg = {};
  msg.hdr.msg_type = kRelease;
  msg.hdr.requester_id = 3;
  msg.hdr.lock_key = 42;

  auto s = mgr.HandleMessage(&msg, sizeof(msg), /*now=*/1000, out);
  ASSERT_STATUS(s, LockStatus::kOk);

  PASS();
}

static void test_key_overflow_exception_callback() {
  TEST(key_overflow_fires_exception_callback);

  LockManager mgr(/*max_keys=*/1);

  bool overflow_fired = false;
  mgr.SetExceptionCallback([&](uint16_t type, uint64_t /*key*/) {
    if (type == hcop::kLockStateOverflow) overflow_fired = true;
  });

  std::vector<OutMessage> out;
  mgr.Acquire(1, 0, 0, 0, out);
  out.clear();

  mgr.Acquire(2, 1, 0, 0, out);
  ASSERT_TRUE(overflow_fired);

  PASS();
}

// ====================================================================
// Main
// ====================================================================

int main() {
  std::printf("=== Lock State Machine Tests ===\n");
  test_acquire_free_key();
  test_acquire_held_key_denied_and_queued();
  test_release_grants_next_waiter();
  test_release_by_non_holder();
  test_idempotent_reacquire();
  test_lease_timeout();
  test_timeout_with_waiters();
  test_key_overflow();
  test_no_queuing_mode();
  test_contention_queue_full();
  test_multiple_independent_keys();
  test_release_unknown_key();
  test_garbage_collection();
  test_handle_message_acquire();
  test_handle_message_release();
  test_key_overflow_exception_callback();
  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
