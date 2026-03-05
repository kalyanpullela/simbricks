/*
 * Lock State Machine — implementation.
 *
 * Location: lib/hcop/ — shared across the entire project.
 */

#include "lock_state.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "hcop_proto.h"

namespace lock {

LockManager::LockManager(uint32_t max_keys, uint64_t default_lease_ns,
                         uint16_t max_waiters_per_key)
    : max_keys_(max_keys),
      default_lease_ns_(default_lease_ns),
      max_waiters_per_key_(max_waiters_per_key) {
}

// ====================================================================
// Message Dispatch
// ====================================================================

LockStatus LockManager::HandleMessage(const void *data, size_t len,
                                      uint64_t now_ns,
                                      std::vector<OutMessage> &out) {
  if (len < sizeof(LockMsgHeader)) return LockStatus::kInvalidMessage;

  const auto *hdr = static_cast<const LockMsgHeader *>(data);

  switch (hdr->msg_type) {
    case kAcquire:
      if (len >= sizeof(AcquireMsg))
        return HandleAcquire(*static_cast<const AcquireMsg *>(data),
                             now_ns, out);
      return LockStatus::kInvalidMessage;
    case kRelease:
      if (len >= sizeof(ReleaseMsg))
        return HandleRelease(*static_cast<const ReleaseMsg *>(data),
                             now_ns, out);
      return LockStatus::kInvalidMessage;
    default:
      return LockStatus::kInvalidMessage;
  }
}

// ====================================================================
// Programmatic API
// ====================================================================

LockStatus LockManager::Acquire(uint64_t key, uint8_t requester_id,
                                uint64_t lease_duration_ns, uint64_t now_ns,
                                std::vector<OutMessage> &out) {
  if (lease_duration_ns == 0) lease_duration_ns = default_lease_ns_;

  KeyState *ks = GetOrCreateKey(key);
  if (!ks) return LockStatus::kKeyOverflow;

  // Key is free — grant immediately.
  if (ks->holder_id == kFree) {
    GrantLock(*ks, key, requester_id, lease_duration_ns, now_ns, out);
    return LockStatus::kGranted;
  }

  // Already held by the same requester — idempotent (refresh lease).
  if (ks->holder_id == requester_id) {
    ks->lease_expiry_ns = now_ns + lease_duration_ns;
    out.push_back(MakeGrant(requester_id, key, ks->lease_expiry_ns));
    return LockStatus::kGranted;
  }

  // Key is held by someone else — check contention queue.
  if (max_waiters_per_key_ == 0) {
    // No queuing allowed — immediate CONTENTION exception.
    if (exception_cb_) {
      exception_cb_(hcop::kLockContention, key);
    }
    out.push_back(MakeDeny(requester_id, key, ks->holder_id));
    return LockStatus::kContention;
  }

  if (ks->waiters.size() >= max_waiters_per_key_) {
    // Queue full — CONTENTION exception.
    if (exception_cb_) {
      exception_cb_(hcop::kLockContention, key);
    }
    out.push_back(MakeDeny(requester_id, key, ks->holder_id));
    return LockStatus::kContention;
  }

  // Check if already in queue (avoid duplicates).
  for (const auto &w : ks->waiters) {
    if (w.requester_id == requester_id) {
      // Already queued — send deny (duplicate request).
      out.push_back(MakeDeny(requester_id, key, ks->holder_id));
      return LockStatus::kDenied;
    }
  }

  // Enqueue the waiter.
  ks->waiters.push_back({requester_id, lease_duration_ns});
  out.push_back(MakeDeny(requester_id, key, ks->holder_id));
  return LockStatus::kDenied;
}

LockStatus LockManager::Release(uint64_t key, uint8_t requester_id,
                                uint64_t now_ns,
                                std::vector<OutMessage> &out) {
  auto it = keys_.find(key);
  if (it == keys_.end()) {
    return LockStatus::kNotHeld;
  }

  KeyState &ks = it->second;
  if (ks.holder_id != requester_id) {
    return LockStatus::kNotHeld;
  }

  // Release the lock.
  ks.holder_id = kFree;
  ks.lease_expiry_ns = 0;

  // Auto-grant to the next waiter.
  if (!GrantNextWaiter(ks, key, now_ns, out)) {
    // No waiters — garbage-collect the key.
    MaybeRemoveKey(key);
  }

  return LockStatus::kOk;
}

// ====================================================================
// Timeout Checking
// ====================================================================

uint32_t LockManager::CheckTimeouts(uint64_t now_ns,
                                    std::vector<OutMessage> &out) {
  uint32_t expired = 0;

  // Collect keys to process (can't modify map while iterating).
  std::vector<uint64_t> expired_keys;
  for (auto &[key, ks] : keys_) {
    if (ks.holder_id != kFree && ks.lease_expiry_ns > 0 &&
        now_ns >= ks.lease_expiry_ns) {
      expired_keys.push_back(key);
    }
  }

  for (uint64_t key : expired_keys) {
    auto it = keys_.find(key);
    if (it == keys_.end()) continue;

    KeyState &ks = it->second;
    if (ks.holder_id == kFree) continue;  // already released concurrently

    // Send TIMEOUT to the expired holder (best-effort, crash-stop assumption).
    out.push_back(MakeTimeout(ks.holder_id, key));

    if (exception_cb_) {
      exception_cb_(hcop::kLockTimeoutCheck, key);
    }

    // Release the lock.
    ks.holder_id = kFree;
    ks.lease_expiry_ns = 0;
    expired++;

    // Auto-grant to the next waiter.
    if (!GrantNextWaiter(ks, key, now_ns, out)) {
      MaybeRemoveKey(key);
    }
  }

  return expired;
}

// ====================================================================
// Accessors
// ====================================================================

const KeyState *LockManager::GetKeyState(uint64_t key) const {
  auto it = keys_.find(key);
  if (it == keys_.end()) return nullptr;
  return &it->second;
}

// ====================================================================
// Internal Handlers
// ====================================================================

LockStatus LockManager::HandleAcquire(const AcquireMsg &msg, uint64_t now_ns,
                                      std::vector<OutMessage> &out) {
  return Acquire(msg.hdr.lock_key, msg.hdr.requester_id,
                 msg.lease_duration_ns, now_ns, out);
}

LockStatus LockManager::HandleRelease(const ReleaseMsg &msg, uint64_t now_ns,
                                      std::vector<OutMessage> &out) {
  return Release(msg.hdr.lock_key, msg.hdr.requester_id, now_ns, out);
}

// ====================================================================
// Internal Helpers
// ====================================================================

KeyState *LockManager::GetOrCreateKey(uint64_t key) {
  auto it = keys_.find(key);
  if (it != keys_.end()) return &it->second;

  if (keys_.size() >= max_keys_) {
    if (exception_cb_) {
      exception_cb_(hcop::kLockStateOverflow, key);
    }
    return nullptr;  // OVERFLOW
  }

  return &keys_[key];
}

void LockManager::GrantLock(KeyState &ks, uint64_t key, uint8_t requester_id,
                            uint64_t lease_duration_ns, uint64_t now_ns,
                            std::vector<OutMessage> &out) {
  ks.holder_id = requester_id;
  ks.lease_expiry_ns = now_ns + lease_duration_ns;
  out.push_back(MakeGrant(requester_id, key, ks.lease_expiry_ns));
}

bool LockManager::GrantNextWaiter(KeyState &ks, uint64_t key, uint64_t now_ns,
                                  std::vector<OutMessage> &out) {
  if (ks.waiters.empty()) return false;

  WaitingRequest next = ks.waiters.front();
  ks.waiters.pop_front();

  GrantLock(ks, key, next.requester_id, next.lease_duration_ns, now_ns, out);
  return true;
}

void LockManager::MaybeRemoveKey(uint64_t key) {
  auto it = keys_.find(key);
  if (it == keys_.end()) return;
  if (it->second.holder_id == kFree && it->second.waiters.empty()) {
    keys_.erase(it);
  }
}

// ====================================================================
// Message Builders
// ====================================================================

OutMessage LockManager::MakeGrant(uint8_t dest, uint64_t key,
                                  uint64_t lease_expiry_ns) {
  GrantMsg msg = {};
  msg.hdr.msg_type = kGrant;
  msg.hdr.requester_id = dest;
  msg.hdr.lock_key = key;
  msg.lease_expiry_ns = lease_expiry_ns;

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage LockManager::MakeDeny(uint8_t dest, uint64_t key,
                                 uint8_t holder_id) {
  DenyMsg msg = {};
  msg.hdr.msg_type = kDeny;
  msg.hdr.requester_id = dest;
  msg.hdr.lock_key = key;
  msg.holder_id = holder_id;

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

OutMessage LockManager::MakeTimeout(uint8_t dest, uint64_t key) {
  TimeoutMsg msg = {};
  msg.hdr.msg_type = kTimeout;
  msg.hdr.requester_id = dest;
  msg.hdr.lock_key = key;

  OutMessage out;
  out.dest_id = dest;
  out.data.assign(reinterpret_cast<uint8_t *>(&msg),
                  reinterpret_cast<uint8_t *>(&msg) + sizeof(msg));
  return out;
}

}  // namespace lock
