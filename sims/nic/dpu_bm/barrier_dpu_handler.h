/*
 * Barrier DPU Handler — bridges BarrierManager state machine to the DPU device.
 *
 * Receives HCOP packets with primitive_type=BARRIER, extracts the barrier
 * payload, feeds it to the state machine, and sends RELEASE responses.
 *
 * This file depends on both lib/hcop/ (shared protocol) and the DPU
 * device model.
 */

#ifndef SIMBRICKS_DPU_BM_BARRIER_DPU_HANDLER_H_
#define SIMBRICKS_DPU_BM_BARRIER_DPU_HANDLER_H_

#include <memory>

#include <hcop/barrier_state.h>

#include "dpu_bm.h"

namespace barrier {

class BarrierDpuHandler : public dpu::PrimitiveHandler {
 public:
  /**
   * @param max_barriers Max tracked barriers (bounded by DPU DRAM).
   */
  explicit BarrierDpuHandler(uint32_t max_barriers = 1'000'000);

  uint16_t PrimitiveType() const override;

  void HandlePacket(dpu::DpuDevice &dev, const void *data, size_t len,
                    dpu::PacketContext &ctx) override;

  /** Access the underlying barrier manager (for testing / telemetry). */
  BarrierManager &Manager() { return mgr_; }
  const BarrierManager &Manager() const { return mgr_; }

  size_t MemoryUsedBytes() const override {
    return mgr_.BarrierCount() * sizeof(BarrierState);
  }

 private:
  BarrierManager mgr_;

  void SendResponses(dpu::DpuDevice &dev,
                     const std::vector<OutMessage> &msgs,
                     const dpu::PacketContext &ctx);

  std::vector<uint8_t> BuildFrame(const OutMessage &msg,
                                  const dpu::PacketContext &ctx);
};

}  // namespace barrier

#endif  // SIMBRICKS_DPU_BM_BARRIER_DPU_HANDLER_H_
