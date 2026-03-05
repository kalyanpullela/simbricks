/*
 * PrimitiveEngine — HCOP fast-path logic dispatch.
 */

#ifndef SIMBRICKS_HCOP_SWITCH_PRIMITIVE_ENGINE_H_
#define SIMBRICKS_HCOP_SWITCH_PRIMITIVE_ENGINE_H_

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

#include <hcop/hcop_proto.h>
#include <hcop/paxos_state.h>
#include <hcop/lock_state.h>
#include <hcop/barrier_state.h>

#include "switch_config.h"

namespace hcop_switch {

struct RoutingDecision {
  enum Action {
    kUnicast,    // Send to dst_ports[0] (SimBricks port index, NOT HCOP node_id)
    kMulticast,  // Send to all ports in dst_ports (empty = flood all except ingress)
    kDrop,       // Do nothing
    kToFallback, // Send to configured fallback port (DPU or host, per placement)
    kBroadcastAll // Send to all ports including ingress
  };
  
  Action action = kDrop;
  std::vector<int> dst_ports; // Port indices
};

class PrimitiveEngine {
 public:
  PrimitiveEngine() = default;
  
  void Init(const SwitchConfig &cfg);

  // Processes packet. Modifies vector in-place (header/payload).
  // Returns routing decision.
  RoutingDecision HandlePacket(std::vector<uint8_t> &pkt, int ingress_port_idx, uint64_t now_ns);

  size_t PaxosUsedSlots() const { return paxos_ ? paxos_->InstanceCount() : 0; }
  size_t LockUsedSlots() const { return locks_ ? locks_->KeyCount() : 0; }
  size_t BarrierUsedSlots() const { return barriers_ ? barriers_->BarrierCount() : 0; }

 private:
  const SwitchConfig *config_ = nullptr; // Pointer to config for lookups

  // State Machines (Managed via unique_ptr because copy/move might overlap?)
  // But lib/hcop state machines are copyable.
  // We'll use unique_ptr to delay construction until Init.
  std::unique_ptr<paxos::PaxosNode> paxos_;
  std::unique_ptr<lock::LockManager> locks_;
  std::unique_ptr<barrier::BarrierManager> barriers_;
  
  // Helpers
  int GetPortForNode(uint8_t node_id) const;
  
  // Handlers per primitive
  RoutingDecision HandlePaxos(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns);
  RoutingDecision HandleLock(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns);
  RoutingDecision HandleBarrier(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns);
};

}  // namespace hcop_switch

#endif  // SIMBRICKS_HCOP_SWITCH_PRIMITIVE_ENGINE_H_

