/*
 * Lock State Machine — tier-agnostic distributed locking implementation.
 *
 * This is the shared library that implements per-key mutual-exclusion
 * with configurable lease duration and contention queuing. It is used
 * by the DPU handler, the host-side program, and (in simplified form)
 * the behavioral switch model. Decision #3: shared library.
 *
 * Protocol: FissLock decomposition (Decision #6).
 * - Per-key mutex, 64-bit key space.
 * - Configurable lease duration (default 10ms).
 * - Contention queue per key (configurable depth, 0 = no queuing).
 * - Lease expiry checking driven by caller.
 *
 * This module is pure state machine logic — no networking, no SimBricks
 * dependencies. Input: a message. Output: zero or more response messages
 * plus a status code.
 *
 * TIMEOUT delivery: Timeout notifications to unreachable holders are
 * best-effort (crash-stop failure assumption per Phase 1 scope; Byzantine
 * fault handling deferred to Experiment 1C).
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_LOCK_STATE_H_
#define SIMBRICKS_HCOP_LOCK_STATE_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "lock_proto.h"

namespace lock {

// ---- Status returned by state machine operations ----
enum class LockStatus : uint8_t {
  kOk = 0,             // Processed normally
  kGranted,            // Lock was granted to the requester
  kDenied,             // Lock is held by another party (ACQUIRE rejected)
  kKeyOverflow,        // Too many tracked keys (STATE_OVERFLOW exception)
  kContention,         // Contention queue full (CONTENTION exception)
  kNotHeld,            // Release by non-holder (no-op)
  kInvalidMessage,     // Message too short or unknown type
};

// ---- Queued acquire request ----
struct WaitingRequest {
  uint8_t requester_id;
  uint64_t lease_duration_ns;
};

// ---- Per-key lock state ----
static constexpr uint8_t kFree = 0xFF;

struct KeyState {
  uint8_t holder_id = kFree;           // who holds the lock (kFree = nobody)
  uint64_t lease_expiry_ns = 0;        // absolute expiry time
  std::deque<WaitingRequest> waiters;  // contention queue
};

// ---- Outbound message ----
// Produced by the state machine; the caller routes to the destination.
struct OutMessage {
  uint8_t dest_id;                // destination node
  std::vector<uint8_t> data;     // serialized message
};

// ---- Exception callback ----
using ExceptionCallback = std::function<void(uint16_t exception_type,
                                             uint64_t lock_key)>;

// ====================================================================
// LockManager — manages a set of per-key locks
// ====================================================================
class LockManager {
 public:
  /**
   * @param max_keys             Maximum number of distinct keys to track.
   *                             When exceeded, returns kKeyOverflow.
   *                             Switch: small (SRAM budget).
   *                             DPU: large (DRAM budget).
   * @param default_lease_ns     Default lease duration when request specifies 0.
   * @param max_waiters_per_key  Maximum contention queue depth per key.
   *                             0 = no queuing (immediate CONTENTION exception
   *                             on any contended acquire). The switch model
   *                             uses 0 — it only does fast-path grant/deny,
   *                             all queuing overflows to DPU.
   */
  LockManager(uint32_t max_keys = 10000,
              uint64_t default_lease_ns = 10'000'000,  // 10ms
              uint16_t max_waiters_per_key = 16);

  /**
   * Process an incoming message, producing zero or more outbound messages.
   *
   * @param data  Raw message bytes (starts with LockMsgHeader).
   * @param len   Length of message.
   * @param now_ns Current time in nanoseconds (for lease computation).
   * @param out   Output vector for response messages.
   * @return LockStatus indicating the result.
   */
  LockStatus HandleMessage(const void *data, size_t len, uint64_t now_ns,
                           std::vector<OutMessage> &out);

  // ---- Programmatic API ----

  /**
   * Attempt to acquire a lock.
   * @return kGranted if acquired, kDenied if held (and queued if room),
   *         kContention if queue full, kKeyOverflow if too many keys.
   */
  LockStatus Acquire(uint64_t key, uint8_t requester_id,
                     uint64_t lease_duration_ns, uint64_t now_ns,
                     std::vector<OutMessage> &out);

  /**
   * Release a lock.
   * @return kOk if released, kNotHeld if not the holder.
   *         If waiters exist, the next one is auto-granted (GRANT sent).
   */
  LockStatus Release(uint64_t key, uint8_t requester_id, uint64_t now_ns,
                     std::vector<OutMessage> &out);

  /**
   * Check for expired leases and auto-release them.
   * Produces TIMEOUT messages for expired holders and GRANT messages
   * for next waiters (if any).
   *
   * @param now_ns  Current time in nanoseconds.
   * @param out     Output vector for messages.
   * @return Number of leases expired.
   */
  uint32_t CheckTimeouts(uint64_t now_ns, std::vector<OutMessage> &out);

  // ---- Accessors ----
  size_t KeyCount() const { return keys_.size(); }
  uint32_t MaxKeys() const { return max_keys_; }
  uint64_t DefaultLeaseNs() const { return default_lease_ns_; }
  uint16_t MaxWaitersPerKey() const { return max_waiters_per_key_; }

  /** Get the state of a specific key (or nullptr if not tracked). */
  const KeyState *GetKeyState(uint64_t key) const;

  /** Set exception callback (for telemetry). */
  void SetExceptionCallback(ExceptionCallback cb) {
    exception_cb_ = std::move(cb);
  }

 private:
  uint32_t max_keys_;
  uint64_t default_lease_ns_;
  uint16_t max_waiters_per_key_;

  std::unordered_map<uint64_t, KeyState> keys_;
  ExceptionCallback exception_cb_;

  // ---- Internal handlers ----
  LockStatus HandleAcquire(const AcquireMsg &msg, uint64_t now_ns,
                           std::vector<OutMessage> &out);
  LockStatus HandleRelease(const ReleaseMsg &msg, uint64_t now_ns,
                           std::vector<OutMessage> &out);

  // Get or create key state. Returns nullptr on overflow.
  KeyState *GetOrCreateKey(uint64_t key);

  // Grant a lock to a requester and produce GRANT message.
  void GrantLock(KeyState &ks, uint64_t key, uint8_t requester_id,
                 uint64_t lease_duration_ns, uint64_t now_ns,
                 std::vector<OutMessage> &out);

  // Auto-grant to next waiter (if any). Returns true if granted.
  bool GrantNextWaiter(KeyState &ks, uint64_t key, uint64_t now_ns,
                       std::vector<OutMessage> &out);

  // Garbage-collect free keys with no waiters.
  void MaybeRemoveKey(uint64_t key);

  // ---- Message builders ----
  OutMessage MakeGrant(uint8_t dest, uint64_t key, uint64_t lease_expiry_ns);
  OutMessage MakeDeny(uint8_t dest, uint64_t key, uint8_t holder_id);
  OutMessage MakeTimeout(uint8_t dest, uint64_t key);
};

}  // namespace lock

#endif  // SIMBRICKS_HCOP_LOCK_STATE_H_
