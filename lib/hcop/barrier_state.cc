/*
 * Barrier State Machine — implementation.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#include "barrier_state.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

#include "hcop_proto.h"
#include <iostream>

namespace barrier {

// ---- Constants ----
static constexpr uint16_t kGenWrap = 65535;

// ---- Constructor ----
BarrierManager::BarrierManager(uint32_t max_barriers, uint16_t default_participants)
    : max_barriers_(max_barriers), default_participants_(default_participants) {}

// ---- SetParticipants ----
BarrierStatus BarrierManager::SetParticipants(uint32_t barrier_id, uint16_t n) {
  if (n < 2 || n > 64) return BarrierStatus::kInvalidConfiguration;

  BarrierState &barrier = barriers_[barrier_id];
  barrier.num_participants = n;
  barrier.current_generation = 0;
  barrier.arrived_count = 0;
  barrier.arrived_bitmap = 0;
  return BarrierStatus::kOk;
}

// ---- HandleMessage ----
BarrierStatus BarrierManager::HandleMessage(const void *data, size_t len,
                                            std::vector<OutMessage> &out) {
  if (len < sizeof(BarrierMsgHeader)) return BarrierStatus::kInvalidMessage;

  const auto *hdr = static_cast<const BarrierMsgHeader *>(data);
  if (hdr->msg_type == kArrive) {
    return Arrive(hdr->barrier_id, hdr->generation, hdr->sender_id, out);
  }

  return BarrierStatus::kInvalidMessage;
}

// ---- Arrive ----
BarrierStatus BarrierManager::Arrive(uint32_t barrier_id, uint16_t generation,
                                     uint8_t sender_id,
                                     std::vector<OutMessage> &out) {
  auto it = barriers_.find(barrier_id);
  if (it == barriers_.end()) {
    // Auto-create with default N if allowed by capacity
    if (barriers_.size() >= max_barriers_) {
      if (exception_cb_) {
        exception_cb_(hcop::kBarrierGenerationOverflow, barrier_id);
      }
      return BarrierStatus::kLayoutOverflow;
    }
    BarrierStatus set_status = SetParticipants(barrier_id, default_participants_);
    if (set_status != BarrierStatus::kOk) {
      return set_status;
    }
    it = barriers_.find(barrier_id);
    if (it == barriers_.end()) {
      return BarrierStatus::kInvalidConfiguration;
    }
  }

  BarrierState &barrier = it->second;

  // Check generation:
  if (generation < barrier.current_generation) {
    // Late arrival for old generation. exception.
    if (exception_cb_) {
      exception_cb_(hcop::kBarrierLateArrival, barrier_id);
    }
    return BarrierStatus::kLateArrival;
  }

  if (generation > barrier.current_generation) {
    // Future generation. Reject.
    return BarrierStatus::kFutureArrival;
  }

  // Check if sender already arrived (idempotency).
  if (sender_id < 64 && (barrier.arrived_bitmap & (1ULL << sender_id))) {
      return BarrierStatus::kDuplicateArrival;
  }

  // Record arrival.
  if (sender_id < 64) {
    barrier.arrived_bitmap |= (1ULL << sender_id);
  }
  barrier.arrived_count++;
  
  // DEBUG
  std::cout << "BarrierManager::Arrive id=" << barrier_id << " gen=" << generation << " sender=" << (int)sender_id << " count=" << barrier.arrived_count << "/" << barrier.num_participants << std::endl;


  // Check if barrier complete.
  if (barrier.arrived_count >= barrier.num_participants) {
    // Release!
    out.push_back(MakeRelease(barrier_id, barrier.current_generation));

    // Advance generation.
    barrier.current_generation++;
    // Wraparound happens naturally for uint16_t, but explicit kGenWrap check
    // isn't harmful if we want to stop at 65535 or wrap earlier.
    // Decision #7 doesn't specify earlier wrap, so full uint16 range used.

    // Reset counts.
    barrier.arrived_count = 0;
    barrier.arrived_bitmap = 0;

    return BarrierStatus::kRelease;
  }

  return BarrierStatus::kOk;
}

// ---- Accessors ----
const BarrierState *BarrierManager::GetBarrier(uint32_t barrier_id) const {
  auto it = barriers_.find(barrier_id);
  if (it == barriers_.end()) return nullptr;
  return &it->second;
}

size_t BarrierManager::MemoryUsedBytes() const {
  return barriers_.size() * sizeof(BarrierState);
}

// ---- Helpers ----
OutMessage BarrierManager::MakeRelease(uint32_t barrier_id, uint16_t gen) {
  ReleaseMsg msg = {};
  msg.hdr.msg_type = kRelease;
  msg.hdr.sender_id = kBroadcast;
  msg.hdr.barrier_id = barrier_id;
  msg.hdr.generation = gen;

  OutMessage out;
  out.dest_id = kBroadcast;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

}  // namespace barrier
