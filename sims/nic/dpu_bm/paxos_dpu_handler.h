/*
 * Paxos DPU Handler — bridges PaxosNode state machine to the DPU device.
 *
 * Receives HCOP packets with primitive_type=PAXOS, extracts the Paxos
 * payload, feeds it to the state machine, and sends responses back
 * via the DPU's Ethernet port.
 *
 * This file depends on both lib/hcop/ (shared protocol) and the DPU
 * device model.
 */

#ifndef SIMBRICKS_DPU_BM_PAXOS_DPU_HANDLER_H_
#define SIMBRICKS_DPU_BM_PAXOS_DPU_HANDLER_H_

#include <memory>

#include <hcop/paxos_state.h>

#include "dpu_bm.h"

namespace paxos {

class PaxosDpuHandler : public dpu::PrimitiveHandler {
 public:
  /**
   * @param node_id      This DPU's replica ID.
   * @param num_replicas Total replicas (3 or 5).
   * @param max_instances Max Paxos instances (bounded by DPU DRAM).
   */
  PaxosDpuHandler(uint8_t node_id, uint16_t num_replicas,
                  uint32_t max_instances = 1000000);

  uint16_t PrimitiveType() const override;

  void HandlePacket(dpu::DpuDevice &dev, const void *data, size_t len,
                    dpu::PacketContext &ctx) override;

  /** Access the underlying Paxos node (for testing / telemetry). */
  PaxosNode &Node() { return node_; }
  const PaxosNode &Node() const { return node_; }

  size_t MemoryUsedBytes() const override {
    return node_.InstanceCount() * sizeof(InstanceState);
  }

 private:
  PaxosNode node_;

  void SendResponses(dpu::DpuDevice &dev,
                     const std::vector<OutMessage> &msgs,
                     const dpu::PacketContext &ctx);

  std::vector<uint8_t> BuildFrame(const OutMessage &msg,
                                  const dpu::PacketContext &ctx);
};

}  // namespace paxos

#endif  // SIMBRICKS_DPU_BM_PAXOS_DPU_HANDLER_H_
