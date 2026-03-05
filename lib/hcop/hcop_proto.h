/*
 * HCOP Protocol Header — shared packet format for heterogeneous
 * coordination primitive placement experiments.
 *
 * This header is used by ALL tiers: the DPU behavioral model, the HCOP
 * switch model, and host-side coordination programs.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_HCOP_PROTO_H_
#define SIMBRICKS_HCOP_HCOP_PROTO_H_

#include <cstdint>

namespace hcop {

// ---- Primitive type identifiers ----
enum PrimitiveType : uint16_t {
  kPrimitivePaxos   = 1,
  kPrimitiveLock    = 2,
  kPrimitiveBarrier = 3,
};

// ---- Per-primitive exception types (Decision #8) ----
// 0 = no exception (normal processing)

enum PaxosException : uint16_t {
  kPaxosNoException     = 0,
  kPaxosLeaderElection  = 1,  // NON-OVERFLOW: non-steady-state leader change
  kPaxosStateOverflow   = 2,  // OVERFLOW: instance log exceeds tier SRAM
  kPaxosConflict        = 3,  // NON-OVERFLOW: concurrent proposals needing arbitration
};

enum LockException : uint16_t {
  kLockNoException     = 0,
  kLockContention      = 1,  // NON-OVERFLOW: key already held, needs queuing
  kLockStateOverflow   = 2,  // OVERFLOW: too many tracked keys for tier SRAM
  kLockTimeoutCheck    = 3,  // NON-OVERFLOW: lease expiry verification needed
};

enum BarrierException : uint16_t {
  kBarrierNoException        = 0,
  kBarrierGenerationOverflow = 1,  // OVERFLOW: too many concurrent barriers
  kBarrierLateArrival        = 2,  // NON-OVERFLOW: arrived after release, needs recovery
};

// General (non-primitive-specific) exception types.
// Values >= 0x0100 to avoid collision with per-primitive exception enums.
enum GeneralException : uint16_t {
  kDpuCoreExhausted  = 0x0100,  // OVERFLOW: DPU ARM core pool full, escalated to host
};

// ---- Source tier identifiers ----
enum SourceTier : uint8_t {
  kTierSwitch = 0,
  kTierDpu    = 1,
  kTierHost   = 2,
};

// ---- Wire format ----
// Sits immediately after the Ethernet header in HCOP protocol packets.
struct HcopHeader {
  uint16_t primitive_type;   // PrimitiveType enum
  uint16_t exception_type;   // per-primitive exception enum, 0 = none
  uint32_t operation_id;     // unique operation identifier
  uint8_t  source_tier;      // SourceTier enum
  uint8_t  num_tier_crossings; // Incremented on each node boundary
  uint32_t tier_path;          // 4-bit nibbles for up to 8 hops (0=Switch, 1=DPU, 2=Host)
  uint16_t payload_len;      // length of primitive-specific payload after header
} __attribute__((packed));

static_assert(sizeof(HcopHeader) == 16,
              "HcopHeader must be exactly 16 bytes (packed wire format)");

// Helper macro to append a tier to the path and increment crossings
#define HCOP_APPEND_PATH(hdr, tier) \
  do { \
    (hdr)->tier_path = ((hdr)->tier_path << 4) | ((tier) & 0x0F); \
    (hdr)->num_tier_crossings++; \
  } while(0)

// ---- HCOP EtherType ----
// Custom EtherType for HCOP protocol packets (in the experimental range).
static constexpr uint16_t kHcopEtherType = 0x88B5;

// Minimum Ethernet frame carrying an HCOP header (no payload):
//   14 (Ethernet) + 16 (HcopHeader) = 30 bytes
static constexpr size_t kMinHcopFrameLen = 14 + sizeof(HcopHeader);

}  // namespace hcop

#endif  // SIMBRICKS_HCOP_HCOP_PROTO_H_
