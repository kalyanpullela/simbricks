/*
 * DPU Configuration — JSON loader and validation.
 */

#include "dpu_config.h"

#include <fstream>
#include <stdexcept>

#include "utils/json.hpp"

using json = nlohmann::json;

namespace dpu {

static void Validate(const DpuConfig &cfg) {
  if (cfg.arm_cores == 0) {
    throw std::runtime_error("DpuConfig: arm_cores must be > 0");
  }
  if (cfg.dram_capacity_mb == 0) {
    throw std::runtime_error("DpuConfig: dram_capacity_mb must be > 0");
  }
  if (cfg.per_packet_base_latency_ns == 0) {
    throw std::runtime_error(
        "DpuConfig: per_packet_base_latency_ns must be > 0");
  }
  if (cfg.service_rate_pps == 0) {
    throw std::runtime_error("DpuConfig: service_rate_pps must be > 0");
  }
  if (cfg.num_replicas < 1) {
    throw std::runtime_error("DpuConfig: num_replicas must be >= 1");
  }
  if (cfg.node_id >= cfg.num_replicas) {
    throw std::runtime_error("DpuConfig: node_id must be < num_replicas");
  }
}

DpuConfig LoadConfig(const std::string &path) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    throw std::runtime_error("DpuConfig: cannot open file: " + path);
  }

  json j;
  try {
    j = json::parse(ifs);
  } catch (const json::parse_error &e) {
    throw std::runtime_error(
        std::string("DpuConfig: malformed JSON: ") + e.what());
  }

  DpuConfig cfg;

  // Each field is optional — missing fields keep the default.
  if (j.contains("arm_cores"))
    cfg.arm_cores = j["arm_cores"].get<uint32_t>();
  if (j.contains("dram_capacity_mb"))
    cfg.dram_capacity_mb = j["dram_capacity_mb"].get<uint64_t>();
  if (j.contains("pcie_gen"))
    cfg.pcie_gen = j["pcie_gen"].get<uint32_t>();
  if (j.contains("pcie_lanes"))
    cfg.pcie_lanes = j["pcie_lanes"].get<uint32_t>();
  if (j.contains("per_packet_base_latency_ns"))
    cfg.per_packet_base_latency_ns =
        j["per_packet_base_latency_ns"].get<uint64_t>();
  if (j.contains("service_rate_pps"))
    cfg.service_rate_pps = j["service_rate_pps"].get<uint64_t>();
  if (j.contains("host_processing_latency_ns"))
    cfg.host_processing_latency_ns =
        j["host_processing_latency_ns"].get<uint64_t>();
  if (j.contains("crypto_accel"))
    cfg.crypto_accel = j["crypto_accel"].get<bool>();
  if (j.contains("doca_flow_steering"))
    cfg.doca_flow_steering = j["doca_flow_steering"].get<bool>();

  if (j.contains("node_id"))
    cfg.node_id = j["node_id"].get<uint8_t>();
  if (j.contains("num_replicas"))
    cfg.num_replicas = j["num_replicas"].get<uint16_t>();
  
  if (j.contains("telemetry_interval_ms"))
    cfg.telemetry_interval_ms = j["telemetry_interval_ms"].get<uint64_t>();

  Validate(cfg);
  return cfg;
}

DpuConfig DefaultConfig() {
  DpuConfig cfg;
  Validate(cfg);  // sanity — defaults must pass validation
  return cfg;
}

}  // namespace dpu
