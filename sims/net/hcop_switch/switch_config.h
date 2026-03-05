/*
 * HCOP Switch Configuration — Tofino-2 resource model and routing table.
 */

#ifndef SIMBRICKS_HCOP_SWITCH_Config_H_
#define SIMBRICKS_HCOP_SWITCH_Config_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hcop_switch {

struct SwitchConfig {
  // ---- Tofino-2 Resource Limits (Aggregate) ----
  // Default: 4 pipelines * 1600 pages/pipe = 6400 pages (16KB each)
  uint32_t sram_pages_total = 6400;
  
  // Default: 4 pipelines * 480 blocks/pipe = 1920 blocks
  uint32_t tcam_blocks_total = 1920;

  // ---- Model Parameters ----
  uint32_t pipeline_stages = 20;
  uint64_t stage_latency_ns = 50;  // 1ns * cycles? Tofino is ~1GHz. 
                                   // But behavioral model uses coarse latency.
                                   // Default to ~1us total for now (20 * 50ns).

  // ---- Primitive Costs (SRAM pages per entry) ----
  double paxos_instance_sram_pages = 0.1; // e.g., 10 instances per page
  double lock_entry_sram_pages = 0.01;    // e.g., 100 locks per page
  double barrier_entry_sram_pages = 0.01; // e.g., 100 barriers per page

  // ---- Primitive Params ----
  uint8_t switch_node_id = 0;   // The node ID of this switch (for Paxos acceptor)
  uint8_t num_replicas = 3;     // Total number of Paxos replicas
  uint8_t barrier_default_participants = 3; // Default N for auto-created barriers

  // ---- Placement Mode ----
  enum PlacementMode {
    kProcessAndForward, // Placements 1,4,5,7: run PrimitiveEngine on HCOP packets
    kForwardOnly,       // Placements 2,3,6: pure L2 forwarding (no PrimitiveEngine)
  };
  PlacementMode placement_mode = kProcessAndForward;

  // ---- Topology / Routing ----
  // Fallback port: where exceptions are forwarded.
  // Set to DPU port for Switch+DPU, host NIC port for Switch+Host.
  // -1 = no fallback (single-tier placement).
  int fallback_port_index = -1;
  std::map<uint8_t, int> node_port_map; // Node ID -> Port Index

  // Helper to load from file
  static SwitchConfig Load(const std::string &path);

  // ---- Telemetry ----
  uint64_t telemetry_interval_ms = 100;
};

}  // namespace hcop_switch

#endif  // SIMBRICKS_HCOP_SWITCH_Config_H_
