/*
 * Lock Wire Protocol — message formats for distributed locking.
 *
 * Per-key mutual-exclusion with configurable lease duration (Decision #6).
 * These structures define the payload that rides inside an HCOP packet
 * (after the HcopHeader). Shared across all tiers (switch, DPU, host).
 *
 * Protocol: FissLock decomposition — fast-path grant/deny on switch,
 * contention queuing on DPU, recovery/timeout on host.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_LOCK_PROTO_H_
#define SIMBRICKS_HCOP_LOCK_PROTO_H_

#include <cstdint>

namespace lock {

// ---- Lock message types ----
enum MessageType : uint8_t {
  kAcquire  = 1,   // Client → Lock manager: request lock
  kGrant    = 2,   // Lock manager → Client: lock acquired
  kRelease  = 3,   // Client → Lock manager: release lock
  kDeny     = 4,   // Lock manager → Client: lock busy (already held)
  kTimeout  = 5,   // Lock manager → Holder: lease revoked
};

// ---- Wire messages ----
// All messages start with a common header, followed by type-specific fields.

struct LockMsgHeader {
  uint8_t  msg_type;       // MessageType enum
  uint8_t  requester_id;   // node ID of requester (0-based)
  uint8_t  _pad[2];        // alignment
  uint64_t lock_key;       // per-key addressing (64-bit key space)
} __attribute__((packed));

static_assert(sizeof(LockMsgHeader) == 12, "LockMsgHeader must be 12 bytes");

// ACQUIRE: request a lock
struct AcquireMsg {
  LockMsgHeader hdr;
  uint64_t lease_duration_ns;  // requested lease duration (0 = use default)
} __attribute__((packed));

// GRANT: lock acquired
struct GrantMsg {
  LockMsgHeader hdr;
  uint64_t lease_expiry_ns;    // absolute timestamp when lease expires
} __attribute__((packed));

// RELEASE: release a lock
struct ReleaseMsg {
  LockMsgHeader hdr;
  // No additional fields needed.
} __attribute__((packed));

// DENY: lock busy (already held by another requester)
struct DenyMsg {
  LockMsgHeader hdr;
  uint8_t holder_id;           // who currently holds the lock
  uint8_t _pad[7];
} __attribute__((packed));

// TIMEOUT: lease revoked (sent to holder)
struct TimeoutMsg {
  LockMsgHeader hdr;
  // No additional fields needed. hdr.requester_id = holder whose lease expired.
} __attribute__((packed));

}  // namespace lock

#endif  // SIMBRICKS_HCOP_LOCK_PROTO_H_
