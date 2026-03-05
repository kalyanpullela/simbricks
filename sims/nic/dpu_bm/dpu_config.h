/*
 * DPU Configuration — hardware capability vector for the BlueField-3
 * behavioral model. Loaded from JSON at startup.
 */

#ifndef SIMBRICKS_DPU_BM_DPU_CONFIG_H_
#define SIMBRICKS_DPU_BM_DPU_CONFIG_H_

#include <cstdint>
#include <string>

namespace dpu {

struct DpuConfig {
  // ---- Compute resources ----
  uint32_t arm_cores = 16;  // concurrent processing slots (BlueField-3: 16 A78)

  // ---- Memory ----
  uint64_t dram_capacity_mb = 16384;  // state storage limit (16 GB default)

  // ---- PCIe parameters ----
  uint32_t pcie_gen = 5;
  uint32_t pcie_lanes = 16;

  // ---- Timing parameters ----
  // CALIBRATION_PLACEHOLDER: 2 µs per-packet DPU ARM processing path
  // Source: published DOCA Flow steering benchmarks for simple forwarding
  uint64_t per_packet_base_latency_ns = 2000;

  // CALIBRATION_PLACEHOLDER: 40 Mpps aggregate across all ARM cores
  // Source: conservative BlueField-3 published specs (packet-size dependent)
  uint64_t service_rate_pps = 40'000'000;

  // CALIBRATION_PLACEHOLDER: 15 µs kernel UDP round-trip overhead
  // Source: mid-range value for standard kernel networking without DPDK
  uint64_t host_processing_latency_ns = 15000;

  // ---- Hardware accelerator flags ----
  bool crypto_accel = true;
  bool doca_flow_steering = true;

  // ---- Identity / Cluster Config ----
  uint8_t node_id = 0;        // 0-based index in the cluster
  uint16_t num_replicas = 3;  // total number of replicas (e.g. 3 or 5)

  // ---- Telemetry ----
  uint64_t telemetry_interval_ms = 100;
};

/**
 * Load configuration from a JSON file.
 *
 * @param path  Path to JSON configuration file.
 * @return Populated DpuConfig struct.
 * @throws std::runtime_error on I/O error, malformed JSON, or invalid values.
 *
 * Validation rules:
 *   - arm_cores must be > 0
 *   - dram_capacity_mb must be > 0
 *   - per_packet_base_latency_ns must be > 0
 *   - service_rate_pps must be > 0
 */
DpuConfig LoadConfig(const std::string &path);

/**
 * Return the built-in default configuration (no file needed).
 */
DpuConfig DefaultConfig();

}  // namespace dpu

#endif  // SIMBRICKS_DPU_BM_DPU_CONFIG_H_
