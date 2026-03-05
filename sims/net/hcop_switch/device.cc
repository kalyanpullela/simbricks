/*
 * HCOP Switch — Device Implementation.
 */

#include "hcop_switch.h"

#include <climits>
#include <cstring>
#include <iostream>

#include <hcop/hcop_proto.h>
#include <hcop/hcop_telemetry.h>

// Extern reference to global netParams defined in switch_main.cc
extern struct SimbricksBaseIfParams netParams;

namespace hcop_switch {

HcopSwitch::HcopSwitch(const SwitchConfig &cfg) : config_(cfg) {
    primitive_engine_.Init(cfg);
    SimbricksNetIfDefaultParams(&netParams);
}

void HcopSwitch::AddPort(NetPort *port) {
    ports_.push_back(port);
}

void HcopSwitch::SchedulePacket(size_t ingress_port, const void *data, size_t len) {
    PacketEvent ev;
    // Calculate dispatch time: current time + pipeline latency
    // SimBricks time is ps (1e-12 s). stage_latency_ns is ns.
    uint64_t latency_ps = config_.pipeline_stages * config_.stage_latency_ns * 1000ULL;
    ev.timestamp = cur_ts_ + latency_ps;
    
    ev.ingress_port = ingress_port;
    ev.data.assign((const uint8_t*)data, (const uint8_t*)data + len);
    
    event_queue_.push(ev);
}

// Helper to transmit packet
void HcopSwitch::ForwardPacket(const void *data, size_t len, int dst_port) {
    if (dst_port >= 0 && (size_t)dst_port < ports_.size()) {
        ports_[dst_port]->TxPacket(data, len, cur_ts_);
    }
}

void HcopSwitch::ProcessEvents() {
    while (!event_queue_.empty()) {
        const PacketEvent &top = event_queue_.top();
        if (top.timestamp > cur_ts_) break;
        
        // Process
        PacketEvent ev = top; // Copy
        event_queue_.pop();
        
        if (pkts_queued_ > 0) pkts_queued_--;
        pkts_processed_++;
        
        // Primitive Engine
        // HandlePacket modifies packet in-place
        // Pass current time in ns (cur_ts_ is ps)
        RoutingDecision d = primitive_engine_.HandlePacket(ev.data, ev.ingress_port, cur_ts_ / 1000ULL);
        
        switch (d.action) {
            case RoutingDecision::kDrop:
                pkts_dropped_++;
                break;
                
            case RoutingDecision::kToFallback:
                // Forward to configured fallback port (DPU or host, per placement)
                if (config_.fallback_port_index >= 0) {
                     ForwardPacket(ev.data.data(), ev.data.size(), config_.fallback_port_index);
                } else {
                     std::cerr << "Warning: fallback port not configured but exception occurred\n";
                }
                break;
                
            case RoutingDecision::kUnicast: {
                if (d.dst_ports.empty()) break; 
                int dst = d.dst_ports[0];
                if (dst >= 0) {
                    ForwardPacket(ev.data.data(), ev.data.size(), dst);
                }
                break;
            }
                
            case RoutingDecision::kMulticast: {
                // If dst_ports empty, broadcast all except ingress
                if (d.dst_ports.empty()) {
                    for (size_t i = 0; i < ports_.size(); ++i) {
                        if (i != ev.ingress_port) {
                            ForwardPacket(ev.data.data(), ev.data.size(), i);
                        }
                    }
                } else {
                    for (int dst : d.dst_ports) {
                        if (dst >= 0) {
                            ForwardPacket(ev.data.data(), ev.data.size(), dst);
                        }
                    }
                }
                break;
            }

            case RoutingDecision::kBroadcastAll: {
                for (size_t i = 0; i < ports_.size(); ++i) {
                    ForwardPacket(ev.data.data(), ev.data.size(), i);
                }
                break;
            }
        }
    }
}

void HcopSwitch::PollPorts() {
    for (size_t i = 0; i < ports_.size(); ++i) {
        auto *p = ports_[i];
        const void *Data;
        size_t Len;
        
        // Drain Rx queue
        while (true) {
            auto res = p->RxPacket(Data, Len, cur_ts_);
            if (res == NetPort::kRxPollSuccess) {
                pkts_queued_++;
                SchedulePacket(i, Data, Len);
                p->RxDone();
            } else {
                break;
            }
        }
    }
}

void HcopSwitch::Run() {
    std::cout << "HcopSwitch running..." << std::endl;
    
    uint64_t sample_interval_ns = config_.telemetry_interval_ms * 1000ULL * 1000ULL;
    std::cout << "HcopSwitch running... interval ns: " << sample_interval_ns << std::endl;
    uint64_t next_sample_ts = hcop::GetTimeNs() + sample_interval_ns;
    
    while (!exiting_) {
        // Send initial syncs to ports waiting for advancement
        for (auto *p : ports_) {
            if (p->IsSync()) {
                p->Sync(cur_ts_);
            }
        }

        uint64_t min_ts = 0;

        do {
            min_ts = ULLONG_MAX;

            PollPorts();
            ProcessEvents();

            // Check next timestamp from synced ports
            for (auto *p : ports_) {
                if (p->IsSync()) {
                    if (min_ts < ULLONG_MAX && p->NextTimestamp() == ULLONG_MAX) {
                        p->Sync(cur_ts_);
                    }
                    uint64_t ts = p->NextTimestamp();
                    if (ts < min_ts) min_ts = ts;
                }
            }
            
            // Check event queue
            if (!event_queue_.empty()) {
                uint64_t ev_ts = event_queue_.top().timestamp;
                if (ev_ts < min_ts) min_ts = ev_ts;
            }

        } while (!exiting_ && (min_ts <= cur_ts_) && min_ts < ULLONG_MAX);

        if (min_ts < ULLONG_MAX) {
            cur_ts_ = min_ts;
        }

        // Telemetry logging is checked independent of tight inner loop
        uint64_t real_time_ns = hcop::GetTimeNs();
        if (real_time_ns >= next_sample_ts) {
            float core_util = 0.0f;
            if (config_.pipeline_stages > 0) {
                core_util = (float)event_queue_.size() / config_.pipeline_stages * 100.0f;
                if (core_util > 100.0f) core_util = 100.0f;
            }
            // Compute in double to avoid truncation of sub-page entries
            // (e.g., barrier_entry_sram_pages = 0.01 → 1 barrier = 0.01 pages)
            double mem_pages = 0.0;
            mem_pages += primitive_engine_.PaxosUsedSlots() * config_.paxos_instance_sram_pages;
            mem_pages += primitive_engine_.LockUsedSlots() * config_.lock_entry_sram_pages;
            mem_pages += primitive_engine_.BarrierUsedSlots() * config_.barrier_entry_sram_pages;
            uint64_t mem_used = (uint64_t)(mem_pages * 4096); // 4KB pages

            // Assume each page is 4KB
            uint64_t mem_cap = config_.sram_pages_total * 4096;

            hcop::TelemetryLogger::Get().LogUtilization(
                real_time_ns, hcop::kTierSwitch, core_util,
                mem_used, mem_cap,
                pkts_processed_, pkts_queued_, pkts_dropped_
            );
            
            next_sample_ts = real_time_ns + sample_interval_ns;
        }
    }
}

} // namespace hcop_switch
