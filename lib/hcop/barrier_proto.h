/*
 * Barrier Wire Protocol — message formats for barrier synchronization.
 *
 * Generation-based counting barrier (N=2 to 64) following Decision #7.
 * Reusable without explicit reset via generation counters.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_BARRIER_PROTO_H_
#define SIMBRICKS_HCOP_BARRIER_PROTO_H_

#include <cstdint>

namespace barrier {

// ---- Barrier message types ----
enum MessageType : uint8_t {
  kArrive  = 1,   // Node → Barrier Manager: arrival at barrier
  kRelease = 2,   // Barrier Manager → Nodes: barrier released (all arrived)
};

// ---- Wire messages ----
// All messages start with a common header.

struct BarrierMsgHeader {
  uint8_t  msg_type;       // MessageType enum
  uint8_t  sender_id;      // node ID of sender (0-based)
  uint16_t generation;     // current generation (wraps at 65535)
  uint32_t barrier_id;     // barrier identifier
} __attribute__((packed));

static_assert(sizeof(BarrierMsgHeader) == 8, "BarrierMsgHeader must be 8 bytes");

// ARRIVE: signal arrival
struct ArriveMsg {
  BarrierMsgHeader hdr;
  // No additional payload needed.
} __attribute__((packed));

// RELEASE: signal release (all arrived)
struct ReleaseMsg {
  BarrierMsgHeader hdr;
  // No additional payload needed. sender_id = manager ID (e.g. 255/Switch/DPU)
} __attribute__((packed));

}  // namespace barrier

#endif  // SIMBRICKS_HCOP_BARRIER_PROTO_H_
