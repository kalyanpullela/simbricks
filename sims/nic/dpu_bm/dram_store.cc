/*
 * Simulated DRAM — key-value store with capacity enforcement.
 */

#include "dpu_bm.h"

#include <cstring>

namespace dpu {

DramStore::DramStore(uint64_t capacity_bytes)
    : capacity_bytes_(capacity_bytes), used_bytes_(0) {
}

bool DramStore::Allocate(uint64_t key, size_t size) {
  // Reject if key already exists.
  if (store_.count(key)) {
    return false;
  }
  // Reject if capacity would be exceeded.
  if (used_bytes_ + size > capacity_bytes_) {
    return false;
  }
  store_[key].resize(size, 0);
  used_bytes_ += size;
  return true;
}

const uint8_t *DramStore::Read(uint64_t key, size_t *out_len) const {
  auto it = store_.find(key);
  if (it == store_.end()) {
    if (out_len) *out_len = 0;
    return nullptr;
  }
  if (out_len) *out_len = it->second.size();
  return it->second.data();
}

bool DramStore::Write(uint64_t key, const uint8_t *data, size_t len) {
  auto it = store_.find(key);
  if (it == store_.end()) {
    return false;
  }
  // Resize if needed (adjusting used_bytes_).
  if (len != it->second.size()) {
    int64_t delta = static_cast<int64_t>(len) -
                    static_cast<int64_t>(it->second.size());
    if (delta > 0 && used_bytes_ + delta > capacity_bytes_) {
      return false;  // would exceed capacity
    }
    used_bytes_ = static_cast<uint64_t>(
        static_cast<int64_t>(used_bytes_) + delta);
    it->second.resize(len);
  }
  std::memcpy(it->second.data(), data, len);
  return true;
}

void DramStore::Free(uint64_t key) {
  auto it = store_.find(key);
  if (it == store_.end()) {
    return;  // no-op on nonexistent key (safe)
  }
  used_bytes_ -= it->second.size();
  store_.erase(it);
}

uint64_t DramStore::UsedBytes() const {
  return used_bytes_;
}

uint64_t DramStore::CapacityBytes() const {
  return capacity_bytes_;
}

}  // namespace dpu
