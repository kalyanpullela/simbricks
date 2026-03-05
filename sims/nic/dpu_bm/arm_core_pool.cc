/*
 * ARM Core Pool — concurrent processing slot allocator.
 *
 * Behavioral model: we track which cores are "busy" via a bitmask.
 * Actual concurrency is simulated through TimedEvents in the Runner,
 * not real threads.
 */

#include "dpu_bm.h"

#include <cassert>

namespace dpu {

ArmCorePool::ArmCorePool(uint32_t capacity)
    : capacity_(capacity), active_(0), in_use_(0) {
  assert(capacity > 0 && capacity <= 64);
}

std::optional<uint32_t> ArmCorePool::TryAcquire() {
  if (active_ >= capacity_) {
    return std::nullopt;
  }
  // Find first clear bit.
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (!(in_use_ & (1ULL << i))) {
      in_use_ |= (1ULL << i);
      ++active_;
      return i;
    }
  }
  // Should not reach here if active_ < capacity_.
  return std::nullopt;
}

void ArmCorePool::Release(uint32_t core_id) {
  assert(core_id < capacity_);
  assert(in_use_ & (1ULL << core_id));
  in_use_ &= ~(1ULL << core_id);
  --active_;
}

uint32_t ArmCorePool::ActiveCount() const {
  return active_;
}

uint32_t ArmCorePool::Capacity() const {
  return capacity_;
}

}  // namespace dpu
