# Implementation Plan — Experiment 1A

| Field | Value |
|-------|-------|
| **Last updated** | 2026-02-18 |
| **Current phase** | Phase 1 (DPU Behavioral Model) |
| **Current task** | Not started |
| **Blockers** | None |

---

## Overview

Build the DPU behavioral model, coordination primitive implementations, hybrid routing logic, telemetry instrumentation, and experiment orchestration for Experiment 1A (Placement Space Exploration) of the HCOP thrust. This is a 3-tier heterogeneous placement experiment (Tofino-2 switch → BlueField-3 DPU → host CPU) testing three coordination primitives (Paxos consensus, distributed locking, barrier synchronization) across 7 placement configurations per primitive (21 total experiments).

## Key Decisions (see `docs/hcop_1a_decisions.md`)

- DPU modeled as a NIC (PCIe to host, single Ethernet to switch) using `nicbm::Runner::Device`
- Behavioral `hcop_switch` model for experiment sweeps; Tofino adapter for validation
- Host-tier programs use kernel UDP sockets (no DPDK)
- Classic Multi-Paxos with stable leader, 3 replicas default
- Per-key mutual-exclusion locking with 10ms lease
- Generation-based counting barrier, N=2 to 64
- Concrete exception types per primitive (not a generic bit)
- Switch → DPU forwarding via dedicated Ethernet port
- Orchestration uses newer `simbricks.orchestration` package

---

## Phase 1: DPU Behavioral Model (`sims/nic/dpu_bm/`)

### 1.1 Configuration System

**Files:** `dpu_config.h`, `dpu_config.cc`

Define the hardware capability vector as a C++ struct, loaded from a JSON file at startup. Uses `nlohmann/json.hpp` (already vendored at `utils/json.hpp` in the SimBricks tree).

```cpp
struct DpuConfig {
  uint32_t arm_cores = 16;                        // concurrent processing slots
  uint64_t dram_capacity_mb = 16384;              // state storage limit
  uint32_t pcie_gen = 5;                          // affects host crossing latency
  uint32_t pcie_lanes = 16;
  uint64_t per_packet_base_latency_ns = 2000;     // CALIBRATION_PLACEHOLDER
  uint64_t service_rate_pps = 40'000'000;         // CALIBRATION_PLACEHOLDER
  bool crypto_accel = true;
  bool doca_flow_steering = true;
};
```

- `LoadConfig(path) → DpuConfig` — parses JSON, validates, throws on invalid input
- Default config embedded for when no JSON file is provided
- Rejects: `arm_cores == 0`, `dram_capacity_mb == 0`, negative latency, malformed JSON

### 1.2 DPU Device Class

**Files:** `dpu_bm.h`, `dpu_bm.cc`

**`class DpuDevice : public nicbm::Runner::Device`** — the main DPU behavioral model.

Overrides from base class:
- `SetupIntro` — PCIe device intro (vendor/device ID, BARs) matching BlueField-3
- `RegRead` / `RegWrite` — status/control registers (core utilization, DRAM usage)
- `DmaComplete` — handles completion of host-bound DMA operations
- `EthRx` — main packet ingress path (see §1.5)
- `Timed` — fires when `ProcessingEvent` completes

Owns: `ArmCorePool`, `DramStore`, primitive handler registry, processing queue.

### 1.3 ARM Core Pool

**Files:** `arm_core_pool.cc` (class defined in `dpu_bm.h`)

Models N concurrent processing slots as a simple available-count model:
- `TryAcquire() → optional<core_id>` — returns a core if available, nullopt if pool exhausted
- `Release(core_id)` — returns core to pool
- `ActiveCount()`, `Capacity()` — for telemetry

This is a behavioral model — we simulate *timing* of concurrent processing via timed events, not actual parallelism. No real threading.

### 1.4 Simulated DRAM

**Files:** `dram_store.cc` (class defined in `dpu_bm.h`)

Key-value store with capacity enforcement:
- `Allocate(key, size) → bool` — reserves DRAM, returns false if capacity exhausted
- `Read(key) → span<uint8_t>` / `Write(key, data) → bool`
- `Free(key)` — releases allocation
- `UsedBytes()`, `CapacityBytes()` — for telemetry

Backed by `std::unordered_map<uint64_t, std::vector<uint8_t>>`.

### 1.5 Packet RX/TX Path

Implemented in `DpuDevice::EthRx` and `DpuDevice::Timed`:

1. Parse HCOP protocol header to extract `primitive_type`
2. Look up registered `PrimitiveHandler`
3. If no handler: log warning, drop packet (graceful — don't crash)
4. `TryAcquire` an ARM core → if pool exhausted: log, drop or escalate to host via PCIe
5. Create `ProcessingEvent` with latency = `per_packet_base_latency_ns` (converted to ps)
6. Schedule event via `runner_->EventSchedule()`
7. On `Timed` callback: call `handler->HandlePacket()`, release ARM core
8. Handler sends response via `runner_->EthSend()` or escalates via `runner_->IssueDma()`

### 1.6 Primitive Handler Interface

**Files:** `dpu_bm.h` (abstract base class)

```cpp
class PrimitiveHandler {
 public:
  virtual uint16_t PrimitiveType() = 0;
  virtual void HandlePacket(DpuDevice&, const void* data, size_t len,
                            PacketContext& ctx) = 0;
  virtual ~PrimitiveHandler() = default;
};
```

- Primitives register at runtime via `DpuDevice::RegisterHandler(unique_ptr<PrimitiveHandler>)`
- Dispatch table: `std::unordered_map<uint16_t, unique_ptr<PrimitiveHandler>>`
- No coupling between DPU model and specific coordination primitive logic

### 1.7 HCOP Protocol Header

**Files:** `hcop_proto.h` (shared with Phase 2 & 3)

```cpp
struct HcopHeader {
  uint16_t primitive_type;   // PAXOS=1, LOCK=2, BARRIER=3
  uint16_t exception_type;   // 0=none, per-primitive enum values
  uint32_t operation_id;     // unique operation identifier
  uint8_t  source_tier;      // 0=switch, 1=DPU, 2=host
  uint8_t  flags;            // reserved
  uint16_t payload_len;      // length of primitive-specific payload
} __attribute__((packed));
```

Exception types per primitive:
- **Paxos:** `LEADER_ELECTION`, `STATE_OVERFLOW`, `CONFLICT`
- **Locking:** `CONTENTION`, `STATE_OVERFLOW`, `TIMEOUT_CHECK`
- **Barrier:** `GENERATION_OVERFLOW`, `LATE_ARRIVAL`

Initially in `dpu_bm/`; moves to shared `include/hcop/` in Phase 2.

### 1.8 PCIe Host Exception Path

When a packet needs host-side processing (overflow, recovery), `DpuDevice` uses `runner_->IssueDma()` to write the packet data into host memory. The host-side driver (implemented in Phase 2 guest programs) polls a ring buffer for incoming exception packets.

### 1.9 Build System Integration

**Files:** `sims/nic/dpu_bm/rules.mk` (new), `sims/nic/rules.mk` (modify)

Follows `i40e_bm/rules.mk` pattern:

```makefile
bin_dpu_bm := $(d)dpu_bm
OBJS := $(addprefix $(d),dpu_bm.o dpu_config.o arm_core_pool.o dram_store.o)
$(bin_dpu_bm): $(OBJS) $(lib_nicbm) $(lib_nicif) $(lib_netif) $(lib_pcie) \
    $(lib_base) -lpthread
```

Add `$(eval $(call subdir,dpu_bm))` to `sims/nic/rules.mk`.

### 1.10 Unit Tests

**Files:** `sims/nic/dpu_bm/tests/test_config.cc`, `test_core_pool.cc`, `test_dram.cc`, `test_main.cc`

Standalone C++ executables (no gtest — matches SimBricks' existing minimal test pattern):
- **Config tests:** valid load, defaults, invalid rejection, malformed JSON
- **Core pool tests:** acquire to capacity, beyond capacity, release, counters
- **DRAM tests:** allocate/read/write/free lifecycle, capacity overflow, nonexistent key, double-free safety

### Design Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | `DpuDevice` extends `Runner::Device` directly, not `SimpleDevice` | DPU needs variable-width register access and custom DMA handling |
| 2 | ARM core pool is a counter, not a thread pool | Behavioral model simulates timing via timed events, not real parallelism |
| 3 | `PrimitiveHandler` is abstract interface, not template | Runtime registration avoids coupling DPU to specific protocols |
| 4 | Config uses `nlohmann/json.hpp` already in-tree | Zero new deps; consistent with `tofino.cc` |
| 5 | Unit tests are standalone C++ (no gtest) | Matches SimBricks' existing `lib/test/parser_test.c` pattern |

---

## Phase 2: Coordination Primitives

### 2.1 Shared Primitive Types

Extract `PrimitiveHandler` base class and HCOP protocol header to `include/hcop/`. Define shared message formats for each primitive's wire protocol.

### 2.2 Paxos (Multi-Paxos with Stable Leader)

- **DPU handler:** `PaxosDpuHandler` — full state machine (prepare/promise/accept/learn, leader election)
- **P4 program:** `paxos.p4` — steady-state accept/learn in switch registers; leader election triggers exception
- **Host program:** `paxos_host.c` — UDP client/server with full protocol, shared state machine library

3 replicas default, configurable to 5. Exception types: `LEADER_ELECTION`, `STATE_OVERFLOW`, `CONFLICT`.

### 2.3 Distributed Locking (Per-Key Mutex with Lease)

- **DPU handler:** `LockDpuHandler` — contention queuing, lease management
- **P4 program:** `lock.p4` — check-and-set on per-key register for fast-path grant
- **Host program:** `lock_host.c` — acquire/release with timeout

10ms default lease. Exception types: `CONTENTION`, `STATE_OVERFLOW`, `TIMEOUT_CHECK`.

### 2.4 Barrier (Generation-Based Counting)

- **DPU handler:** `BarrierDpuHandler` — overflow management for concurrent barriers
- **P4 program:** `barrier.p4` — arrival counting per generation, broadcast release
- **Host program:** `barrier_host.c` — arrive/wait with generation tracking

N=2 to 64. Exception types: `GENERATION_OVERFLOW`, `LATE_ARRIVAL`.

### 2.5 Unit Tests per Primitive

Test each protocol state machine in isolation (not through SimBricks):
- Paxos: prepare/promise/accept/learn sequences, leader election, conflicting proposals
- Locking: grant/release, contention queuing, timeout/expiry
- Barrier: arrival counting, release notification, late arrivals

---

## Phase 3: Hybrid Routing & Exception Forwarding

### 3.1 HCOP Behavioral Switch Model (`sims/net/hcop_switch/`)

Behavioral switch that models Tofino-2 P4 pipeline resources (stages, SRAM, TCAM, ALU) with configurable latency. Parameterized from P4 compiler resource reports (from Open P4 Studio).

### 3.2 Exception Routing Logic

Switch fast-path handles common cases at line rate. Exception detection:
- Resource overflow (SRAM page limit exceeded)
- Protocol state requiring arbitration (leader election, contention)
- Unsupported operation for P4 ALU model

Exception packets forwarded to DPU's dedicated switch port with exception type header set.

### 3.3 DPU → Host Overflow Path

When DPU processing queue is full or DRAM exhausted, escalate to host via PCIe DMA.

### 3.4 Integration Tests

Minimal SimBricks simulation: 2 QEMU hosts + 1 DPU + 1 switch. Verify:
- Single Paxos round end-to-end
- Lock acquire/release round-trip
- Barrier with N=2

---

## Phase 4: Telemetry & Metrics Collection

### 4.1 Per-Operation Metrics

CSV with: `operation_id`, `primitive_type`, `placement_config`, `start_time_ns`, `end_time_ns`, `latency_ns`, `tier_path`, `num_tier_crossings`, `was_exception`, `was_overflow`

### 4.2 Per-Tier Resource Utilization

Sampled every T ms: `timestamp_ns`, `tier`, `core_utilization_pct`, `memory_used_bytes`, `packets_processed`, `packets_queued`, `packets_dropped`

### 4.3 Aggregate Summary

One row per experiment run: `latency_mean/p50/p99`, `throughput_ops_per_sec`, utilization percentages, `exception_rate_pct`, `overflow_rate_pct`

### 4.4 Implementation Notes

- CSV logging buffers writes, doesn't flush per-packet
- Warm-up period (configurable N seconds) filtered from metrics
- All telemetry hooks added to DPU model, switch model, and host programs

---

## Phase 5: Experiment Orchestration

### 5.1 Python Scripts (`experiments/pyexps/hcop/`)

Uses `simbricks.orchestration` package (system/simulation/instantiation pattern from `simple_demo.py`).

### 5.2 21-Configuration Sweep

For each primitive (Paxos, Locking, Barrier) × 7 placements:

| # | Placement | What runs where |
|---|-----------|----------------|
| 1 | Switch-only | All logic in P4 on Tofino-2 |
| 2 | DPU-only | All logic in DPU behavioral model |
| 3 | Host-only | All logic in QEMU guest (baseline) |
| 4 | Switch+DPU | Fast path on switch, exceptions to DPU |
| 5 | Switch+Host | Fast path on switch, exceptions to host |
| 6 | DPU+Host | Primary on DPU, overflow to host |
| 7 | Switch+DPU+Host | Full 3-tier hybrid |

### 5.3 Parallel Execution

Experiment scripts support parallel execution where SimBricks allows.

### 5.4 Results Aggregation

Collect per-run CSVs, generate aggregate summaries, produce comparison plots.
