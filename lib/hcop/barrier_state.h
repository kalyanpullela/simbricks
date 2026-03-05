/*
 * Barrier State Machine — tier-agnostic barrier synchronization.
 *
 * This is the shared library that implements generation-based counting
 * barriers. Used by DPU handler and host-side programs.
 *
 * Protocol: Decision #7.
 * - N=2 to 64 participants.
 * - Generation counter wraps at 65535.
 * - ARRIVE increments count; when count==N → RELEASE + bump generation.
 * - Late arrivals (gen < current) detected as exceptions.
 * - Future arrivals (gen > current) rejected (kFutureArrival).
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#ifndef SIMBRICKS_HCOP_BARRIER_STATE_H_
#define SIMBRICKS_HCOP_BARRIER_STATE_H_

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>

#include "barrier_proto.h"

namespace barrier {

// ---- Status returned by state machine operations ----
enum class BarrierStatus : uint8_t {
  kOk = 0,                // Processed normally (partial arrival)
  kRelease,               // Barrier released (count reached N)
  kLateArrival,           // Late arrival for old generation (exception)
  kFutureArrival,         // Arrival for future generation (rejected)
  kDuplicateArrival,      // Already arrived in current generation (idempotent)
  kLayoutOverflow,        // Too many barriers tracked (exception)
  kInvalidMessage,        // Malformed message
  kInvalidConfiguration,  // Invalid N (must be 2-64)
};

// ---- Per-barrier state ----
struct BarrierState {
  uint16_t current_generation = 0;
  uint16_t num_participants = 0;  // N
  uint16_t arrived_count = 0;
  uint64_t arrived_bitmap = 0;    // tracks arrivals (up to 64 nodes)
};

// ---- Outbound message ----
struct OutMessage {
  uint8_t dest_id;  // 255 = broadcast to all participants
  std::vector<uint8_t> data;
};

static constexpr uint8_t kBroadcast = 255;

// ---- Exception callback ----
using ExceptionCallback = std::function<void(uint16_t exception_type,
                                             uint32_t barrier_id)>;

// ====================================================================
// BarrierManager — manages a set of barriers
// ====================================================================
class BarrierManager {
 public:
  /**
   * @param max_barriers Max number of distinct barriers to track.
   */
  explicit BarrierManager(uint32_t max_barriers = 1000, uint16_t default_participants = 3);

  /**
   * Configure a barrier with N participants.
   * Must be called before first arrival.
   * @return kInvalidConfiguration if N not in [2, 64].
   */
  BarrierStatus SetParticipants(uint32_t barrier_id, uint16_t n);

  /**
   * Process an incoming message.
   * If returns kRelease, 'out' contains the RELEASE message (broadcast).
   */
  BarrierStatus HandleMessage(const void *data, size_t len,
                              std::vector<OutMessage> &out);

  /**
   * Register arrival for a node.
   * @return kRelease if this completes the barrier.
   */
  BarrierStatus Arrive(uint32_t barrier_id, uint16_t generation,
                       uint8_t sender_id, std::vector<OutMessage> &out);

  // ---- Accessors ----
  size_t BarrierCount() const { return barriers_.size(); }
  uint32_t MaxBarriers() const { return max_barriers_; }
  size_t MemoryUsedBytes() const;
  const BarrierState *GetBarrier(uint32_t barrier_id) const;

  void SetExceptionCallback(ExceptionCallback cb) {
    exception_cb_ = std::move(cb);
  }

 private:
  uint32_t max_barriers_;
  uint16_t default_participants_;
  std::unordered_map<uint32_t, BarrierState> barriers_;
  ExceptionCallback exception_cb_;

  // Helpers
  BarrierState *GetOrCreateBarrier(uint32_t barrier_id);
  OutMessage MakeRelease(uint32_t barrier_id, uint16_t generation);
};

}  // namespace barrier

#endif  // SIMBRICKS_HCOP_BARRIER_STATE_H_
