/*
 * Lock DPU Handler — bridges LockManager state machine to the DPU device.
 *
 * Receives HCOP packets with primitive_type=LOCK, extracts the lock
 * payload, feeds it to the state machine, and sends responses back
 * via the DPU's Ethernet port.
 *
 * This file depends on both lib/hcop/ (shared protocol) and the DPU
 * device model.
 */

#ifndef SIMBRICKS_DPU_BM_LOCK_DPU_HANDLER_H_
#define SIMBRICKS_DPU_BM_LOCK_DPU_HANDLER_H_

#include <memory>

#include <hcop/lock_state.h>

#include "dpu_bm.h"

namespace lock {

class LockDpuHandler : public dpu::PrimitiveHandler {
 public:
  /**
   * @param max_keys             Max tracked keys (bounded by DPU DRAM).
   * @param default_lease_ns     Default lease duration (10ms).
   * @param max_waiters_per_key  Contention queue depth per key.
   */
  LockDpuHandler(uint32_t max_keys = 1'000'000,
                 uint64_t default_lease_ns = 10'000'000,
                 uint16_t max_waiters_per_key = 16);

  uint16_t PrimitiveType() const override;

  void HandlePacket(dpu::DpuDevice &dev, const void *data, size_t len,
                    dpu::PacketContext &ctx) override;

  /** Access the underlying lock manager (for testing / telemetry). */
  LockManager &Manager() { return mgr_; }
  const LockManager &Manager() const { return mgr_; }

  size_t MemoryUsedBytes() const override {
    return mgr_.KeyCount() * sizeof(KeyState);
  }

 private:
  LockManager mgr_;

  void SendResponses(dpu::DpuDevice &dev,
                     const std::vector<OutMessage> &msgs,
                     const dpu::PacketContext &ctx);

  std::vector<uint8_t> BuildFrame(const OutMessage &msg,
                                  const dpu::PacketContext &ctx);
};

}  // namespace lock

#endif  // SIMBRICKS_DPU_BM_LOCK_DPU_HANDLER_H_
