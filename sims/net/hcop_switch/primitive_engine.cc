/*
 * PrimitiveEngine — Implementation with full logic.
 */

#include "primitive_engine.h"

#include <cstring>
#include <iostream>
#include <algorithm>

#include <hcop/hcop_proto.h>
#include <hcop/paxos_proto.h>
#include <hcop/lock_proto.h>
#include <hcop/barrier_proto.h>

namespace hcop_switch {

int PrimitiveEngine::GetPortForNode(uint8_t node_id) const {
  if (!config_) return -1;
  auto it = config_->node_port_map.find(node_id);
  if (it != config_->node_port_map.end()) {
    return it->second;
  }
  return -1;
}

void PrimitiveEngine::Init(const SwitchConfig &cfg) {
    config_ = &cfg;
    
    double paxos_pages = cfg.sram_pages_total * 0.4;
    double lock_pages = cfg.sram_pages_total * 0.3;
    double barrier_pages = cfg.sram_pages_total * 0.3;

    size_t paxos_capacity = (cfg.paxos_instance_sram_pages > 0) ? (size_t)(paxos_pages / cfg.paxos_instance_sram_pages) : 10000;
    size_t lock_capacity = (cfg.lock_entry_sram_pages > 0) ? (size_t)(lock_pages / cfg.lock_entry_sram_pages) : 10000;
    size_t barrier_capacity = (cfg.barrier_entry_sram_pages > 0) ? (size_t)(barrier_pages / cfg.barrier_entry_sram_pages) : 10000;

    if (paxos_capacity < 1) paxos_capacity = 1;
    if (lock_capacity < 1) lock_capacity = 1;
    if (barrier_capacity < 1) barrier_capacity = 1;

    paxos_ = std::make_unique<paxos::PaxosNode>(cfg.switch_node_id, cfg.num_replicas, paxos_capacity);
    locks_ = std::make_unique<lock::LockManager>(lock_capacity, 10000000, 0); 
    barriers_ = std::make_unique<barrier::BarrierManager>(barrier_capacity, cfg.barrier_default_participants);
}

RoutingDecision PrimitiveEngine::HandlePacket(std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns) {
    RoutingDecision d;
    d.action = RoutingDecision::kDrop;

    if (pkt.size() < 14) return d;

    uint16_t etype = (pkt[12] << 8) | pkt[13];
    if (etype == hcop::kHcopEtherType) {
        std::cout << "HandlePacket: matched HCOP etype=0x88b5 from ingress=" << ingress_port << std::endl;
    }
    if (etype != hcop::kHcopEtherType) {
        // Non-HCOP traffic: L2 flood.
        d.action = RoutingDecision::kMulticast;
        return d; 
    }
    
    if (pkt.size() < 14 + sizeof(hcop::HcopHeader)) {
        std::cout << "Size too small: " << pkt.size() << std::endl;
        return d;
    }
    
    auto *hdr = reinterpret_cast<hcop::HcopHeader *>(pkt.data() + 14);
    HCOP_APPEND_PATH(hdr, hcop::kTierSwitch);
    std::cout << "HcopHeader: prim=" << (int)hdr->primitive_type << " exc=" << (int)hdr->exception_type << " size=" << pkt.size() << std::endl;

    // ---- Placement mode: kForwardOnly ----
    // Switch does not run state machines. Forwards HCOP to fallback port
    // or L2-floods responses coming back from the fallback tier.
    if (config_->placement_mode == SwitchConfig::kForwardOnly) {
        if (config_->fallback_port_index >= 0 &&
            ingress_port == config_->fallback_port_index) {
            // Response from fallback tier -> L2 flood to clients
            d.action = RoutingDecision::kMulticast;
        } else if (config_->fallback_port_index >= 0) {
            // Client request -> forward to fallback port
            d.action = RoutingDecision::kToFallback;
        } else {
            // No fallback configured -> L2 flood (host-only style)
            d.action = RoutingDecision::kMulticast;
        }
        return d;
    }

    // ---- Placement mode: kProcessAndForward ----
    // Packet from fallback port might be a response from a lower tier.
    // Only L2-flood actual responses to clients; do NOT re-process them through state machines.
    // Client requests sent by dual-mode fallback nodes must be processed by the primitive engine.
    if (config_->fallback_port_index >= 0 &&
        ingress_port == config_->fallback_port_index) {
        
        bool is_response = false;
        switch (hdr->primitive_type) {
            case hcop::kPrimitiveBarrier: {
                auto *bhdr = reinterpret_cast<barrier::BarrierMsgHeader *>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
                if (bhdr->msg_type == barrier::kRelease) is_response = true;
                break;
            }
            case hcop::kPrimitiveLock: {
                auto *lhdr = reinterpret_cast<lock::LockMsgHeader *>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
                if (lhdr->msg_type == lock::kGrant || lhdr->msg_type == lock::kDeny || lhdr->msg_type == lock::kTimeout) is_response = true;
                break;
            }
            case hcop::kPrimitivePaxos: {
                // For Paxos, the server is just a replica. Client requests are kPrepare/kAccept.
                // Assuming everything else is a response for bypassing.
                auto *phdr = reinterpret_cast<uint8_t *>(pkt.data() + 14 + sizeof(hcop::HcopHeader)); 
                if (phdr[0] != 1 && phdr[0] != 3) is_response = true; // 1=kPrepare, 3=kAccept
                break;
            }
            default:
                break;
        }
        
        if (is_response) {
            d.action = RoutingDecision::kMulticast;
            return d;
        }
    }
    
    // Packets with exception_type already set: forward to fallback.
    if (hdr->exception_type != 0) {
        if (config_->fallback_port_index >= 0) {
            d.action = RoutingDecision::kToFallback;
        } else {
            d.action = RoutingDecision::kDrop;
        }
        return d;
    }

    switch (hdr->primitive_type) {
        case hcop::kPrimitivePaxos:
            return HandlePaxos(hdr, pkt, ingress_port, now_ns);
        case hcop::kPrimitiveLock:
            return HandleLock(hdr, pkt, ingress_port, now_ns);
        case hcop::kPrimitiveBarrier:
            return HandleBarrier(hdr, pkt, ingress_port, now_ns);
        default:
            d.action = RoutingDecision::kDrop;
            return d;
    }
}

static void UpdatePacketPayload(std::vector<uint8_t> &pkt, const std::vector<uint8_t> &payload) {
    size_t new_total = 14 + sizeof(hcop::HcopHeader) + payload.size();
    pkt.resize(new_total);
    std::memcpy(pkt.data() + 14 + sizeof(hcop::HcopHeader), payload.data(), payload.size());
    auto *hdr = reinterpret_cast<hcop::HcopHeader *>(pkt.data() + 14);
    hdr->payload_len = payload.size();

    // Prevent client NIC loopback drop by setting a dummy switch Source MAC
    pkt[6] = 0x02; pkt[7] = 0x00; pkt[8] = 0x53; pkt[9] = 0x57; pkt[10] = 0x54; pkt[11] = 0x00;
}


// Inline Logic
#define AGGREGATE_ROUTING(OutType) \
    const auto &msg0 = out[0]; \
    UpdatePacketPayload(pkt, msg0.data); \
    d.dst_ports.clear(); \
    bool broadcast = false; \
    for (const auto &m : out) { \
        if (m.dest_id == 255) broadcast = true; \
        else { \
            int p = GetPortForNode(m.dest_id); \
            if (p >= 0) d.dst_ports.push_back(p); \
        } \
    } \
    if (broadcast) { \
        d.action = RoutingDecision::kBroadcastAll; \
        d.dst_ports.clear(); \
    } else if (d.dst_ports.size() > 1) { \
        d.action = RoutingDecision::kMulticast; \
    } else if (d.dst_ports.size() == 1) { \
        d.action = RoutingDecision::kUnicast; \
        /* dst_ports already has 1 element */ \
    } else { \
        d.action = RoutingDecision::kDrop; \
    }

RoutingDecision PrimitiveEngine::HandlePaxos(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns) {
    (void)now_ns;
    RoutingDecision d;
    std::vector<paxos::OutMessage> out;
    
    const uint8_t *in_payload = pkt.data() + 14 + sizeof(hcop::HcopHeader);
    size_t in_len = pkt.size() - (14 + sizeof(hcop::HcopHeader));
    
    paxos::PaxosStatus status = paxos_->HandleMessage(in_payload, in_len, out);
    
    std::cout << "HandlePaxos: Status=" << (int)status << " out.size=" << out.size() << std::endl;
    
    if (status != paxos::PaxosStatus::kOk) {
        if (status == paxos::PaxosStatus::kInstanceOverflow) hdr->exception_type = hcop::kPaxosStateOverflow;
        
        if (hdr->exception_type != 0) {
            d.action = RoutingDecision::kToFallback;
            return d;
        }
        d.action = RoutingDecision::kDrop;
        return d;
    }
    
    if (out.empty()) {
        d.action = RoutingDecision::kDrop;
        return d;
    }
    
    AGGREGATE_ROUTING(paxos::OutMessage);
    return d;
}

RoutingDecision PrimitiveEngine::HandleLock(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns) {
    RoutingDecision d;
    std::vector<lock::OutMessage> out;
    
    lock::LockMsgHeader *lhdr = reinterpret_cast<lock::LockMsgHeader *>(pkt.data() + 14 + sizeof(hcop::HcopHeader));
    
    const uint8_t *in_payload = pkt.data() + 14 + sizeof(hcop::HcopHeader);
    size_t in_len = pkt.size() - (14 + sizeof(hcop::HcopHeader));
    
    lock::LockStatus status = locks_->HandleMessage(in_payload, in_len, now_ns, out);
    
    if (status != lock::LockStatus::kOk && status != lock::LockStatus::kGranted && status != lock::LockStatus::kNotHeld) {
        if (status == lock::LockStatus::kContention) hdr->exception_type = hcop::kLockContention;
        else if (status == lock::LockStatus::kKeyOverflow) hdr->exception_type = hcop::kLockStateOverflow;
        
        if (status == lock::LockStatus::kDenied) {
             // Not exception
        } else if (hdr->exception_type != 0) {
             d.action = RoutingDecision::kToFallback;
             return d;
        }
    }
    
    if (out.empty()) {
        if (lhdr->msg_type == lock::kRelease && config_->fallback_port_index >= 0) {
            d.action = RoutingDecision::kToFallback;
            return d;
        }
        d.action = RoutingDecision::kDrop;
        return d;
    }
    
    AGGREGATE_ROUTING(lock::OutMessage);
    return d;
}

RoutingDecision PrimitiveEngine::HandleBarrier(hcop::HcopHeader *hdr, std::vector<uint8_t> &pkt, int ingress_port, uint64_t now_ns) {
    (void)now_ns;
    RoutingDecision d;
    std::vector<barrier::OutMessage> out;
    
    const uint8_t *in_payload = pkt.data() + 14 + sizeof(hcop::HcopHeader);
    size_t in_len = pkt.size() - (14 + sizeof(hcop::HcopHeader));

    barrier::BarrierStatus status = barriers_->HandleMessage(in_payload, in_len, out);

    if (status != barrier::BarrierStatus::kOk && status != barrier::BarrierStatus::kRelease) {
        if (status == barrier::BarrierStatus::kLateArrival) hdr->exception_type = hcop::kBarrierLateArrival;
        else if (status == barrier::BarrierStatus::kFutureArrival) {
             d.action = RoutingDecision::kDrop;
             return d;
        } else if (status == barrier::BarrierStatus::kLayoutOverflow) {
             hdr->exception_type = hcop::kBarrierGenerationOverflow;
        }
        
        if (hdr->exception_type != 0) {
             d.action = RoutingDecision::kToFallback;
             return d;
        }
        
        d.action = RoutingDecision::kDrop;
        return d;
    }
    
    if (out.empty()) {
        d.action = RoutingDecision::kDrop;
        return d;
    }
    
    AGGREGATE_ROUTING(barrier::OutMessage);
    return d;
}

} // namespace
