/*
 * HCOP Switch Configuration — implementation.
 */

#include "switch_config.h"

#include <fstream>
#include <stdexcept>
#include <iostream>

#include "utils/json.hpp"

using json = nlohmann::json;

namespace hcop_switch {

SwitchConfig SwitchConfig::Load(const std::string &path) {
  SwitchConfig cfg;
  std::ifstream f(path);
  if (!f.is_open()) {
    throw std::runtime_error("SwitchConfig: failed to open config file: " + path);
  }

  json j;
  f >> j;

  // Resource limits
  if (j.contains("sram_pages_total")) {
    cfg.sram_pages_total = j["sram_pages_total"];
  }
  if (j.contains("tcam_blocks_total")) {
    cfg.tcam_blocks_total = j["tcam_blocks_total"];
  }

  // Model params
  if (j.contains("pipeline_stages")) {
    cfg.pipeline_stages = j["pipeline_stages"];
  }
  if (j.contains("stage_latency_ns")) {
    cfg.stage_latency_ns = j["stage_latency_ns"];
  }

  // Primitive costs
  if (j.contains("paxos_instance_sram_pages")) {
    cfg.paxos_instance_sram_pages = j["paxos_instance_sram_pages"];
  }
  if (j.contains("lock_entry_sram_pages")) {
    cfg.lock_entry_sram_pages = j["lock_entry_sram_pages"];
  }
  if (j.contains("barrier_entry_sram_pages")) {
    cfg.barrier_entry_sram_pages = j["barrier_entry_sram_pages"];
  }

  // Topology
  if (j.contains("topology")) {
    const auto &topo = j["topology"];
    // Accept both old (dpu_port_index) and new (fallback_port_index) key names
    if (topo.contains("fallback_port_index")) {
      cfg.fallback_port_index = topo["fallback_port_index"];
    } else if (topo.contains("dpu_port_index")) {
      cfg.fallback_port_index = topo["dpu_port_index"];
    }
    
    if (topo.contains("nodes")) {
      for (const auto &node : topo["nodes"]) {
        uint8_t id = node["id"];
        int port = node["port"];
        cfg.node_port_map[id] = port;
      }
    }
  }

  // Placement mode
  if (j.contains("placement_mode")) {
    std::string mode = j["placement_mode"];
    if (mode == "forward_only") {
      cfg.placement_mode = SwitchConfig::kForwardOnly;
    } else {
      cfg.placement_mode = SwitchConfig::kProcessAndForward;
    }
  }

  // Validation
  if (cfg.sram_pages_total == 0) {
    throw std::runtime_error("SwitchConfig: sram_pages_total must be > 0");
  }
  if (cfg.tcam_blocks_total == 0) {
    throw std::runtime_error("SwitchConfig: tcam_blocks_total must be > 0");
  }
  if (cfg.fallback_port_index < 0) {
    std::cerr << "SwitchConfig: info: fallback_port_index not configured (-1), single-tier mode\n";
  }

  // Primitive params
  if (j.contains("num_replicas")) {
    cfg.num_replicas = j["num_replicas"];
  }
  if (j.contains("switch_node_id")) {
    cfg.switch_node_id = j["switch_node_id"];
  }
  if (j.contains("barrier_default_participants")) {
    cfg.barrier_default_participants = j["barrier_default_participants"];
    std::cout << "SwitchConfig::Load parsing JSON barrier_default_participants=" << j["barrier_default_participants"] 
              << " set to " << (int)cfg.barrier_default_participants << std::endl;
  }

  if (j.contains("telemetry_interval_ms")) {
    cfg.telemetry_interval_ms = j["telemetry_interval_ms"];
  }

  return cfg;
}

}  // namespace hcop_switch
