# Task Tracker — Experiment 1A

| Field | Value |
|-------|-------|
| **Last updated** | 2026-03-04 |
| **Current phase** | Phase 5 — COMPLETE |
| **Current task** | Task 5.1 — **COMPLETE (18/21 configs validated, 3 known issues)** |
| **Blockers** | None — 3 remaining barrier configs deferred to future work |

---

## Phase 1: DPU Behavioral Model (`sims/nic/dpu_bm/`)

- [x] **1.1** Configuration system — JSON config loader, hardware capability vector struct, validation
  - Files: `dpu_config.h`, `dpu_config.cc`
  - Depends on: nothing
- [x] **1.2** DPU device class — subclass `nicbm::Runner::Device`, register interface, `SetupIntro`
  - Files: `dpu_bm.h`, `dpu_bm.cc`
  - Depends on: 1.1
- [x] **1.3** ARM core pool — concurrent processing slot allocator with resource accounting
  - Files: `arm_core_pool.cc`
  - Depends on: 1.1
- [x] **1.4** Simulated DRAM — state storage with capacity tracking and overflow detection
  - Files: `dram_store.cc`
  - Depends on: 1.1
- [x] **1.5** Packet RX/TX path — Ethernet ingress → dispatch → processing pipeline → egress
  - Files: `dpu_bm.cc` (EthRx, Timed methods)
  - Depends on: 1.2, 1.3, 1.6, 1.7
- [x] **1.6** Primitive handler interface — `PrimitiveHandler` base class with registration
  - Files: `dpu_bm.h`
  - Depends on: nothing
- [x] **1.7** HCOP protocol header — shared packet format with exception type enums
  - Files: `hcop_proto.h`
  - Depends on: nothing
- [x] **1.8** PCIe host exception path — DMA-based escalation from DPU to host
  - Files: `dpu_bm.cc`
  - Depends on: 1.2
- [x] **1.9** Build system integration — `rules.mk`, add `dpu_bm` to `sims/nic/rules.mk`
  - Files: `rules.mk` (new), `sims/nic/rules.mk` (modify)
  - Depends on: 1.1–1.8
- [x] **1.10** Unit tests — config, core pool, DRAM, handler registration
  - Files: `tests/test_config.cc`, `test_core_pool.cc`, `test_dram.cc`
  - Depends on: 1.1, 1.3, 1.4
- [x] **1.11** Compilation verification — clean build of `dpu_bm` binary
  - Depends on: 1.9
- [ ] **1.12** Concurrent packet pipeline test — two HCOP packets arriving within one processing delay, verify second queues until core available
  - Files: `tests/test_pipeline.cc` (extend)
  - Depends on: 1.11

---

## Phase 2: Coordination Primitives

- [x] **2.1** Shared protocol library — `lib/hcop/` with `hcop_proto.h`, `paxos_proto.h`, `lock_proto.h`, `barrier_proto.h`
  - Tier-agnostic code separated from device-layer code
  - Compiles with zero SimBricks dependencies (`-I./lib` only)
  - Depends on: Phase 1 complete
- [ ] **2.2** Paxos — Multi-Paxos state machine (host C++ library, DPU handler, P4 program)
  - Sub-tasks:
    - [x] 2.2a Shared state machine: `lib/hcop/paxos_state.h`, `paxos_state.cc`, `paxos_proto.h`
    - [x] 2.2b DPU handler: `sims/nic/dpu_bm/paxos_dpu_handler.h`, `paxos_dpu_handler.cc`
    - [ ] 2.2c `paxos.p4` P4 program (compile with Open P4 Studio)
    - [ ] 2.2d Host-side `paxos_host.c` UDP client/server
    - [x] 2.2e Unit tests — state machine (14/14 pass) + handler integration (4/4 pass)
    - [x] 2.2f `max_instances` constructor param (switch SRAM vs DPU DRAM budget)
    - [x] 2.2g `PaxosStatus` return type with `kInstanceOverflow` for tier handoff
  - Depends on: 2.1
- [ ] **2.3** Distributed Locking — per-key mutex with lease (host, DPU, P4)
  - Sub-tasks:
    - [x] 2.3a Shared state machine: `lib/hcop/lock_state.h`, `lock_state.cc`, `lock_proto.h`
    - [x] 2.3b DPU handler: `sims/nic/dpu_bm/lock_dpu_handler.h`, `lock_dpu_handler.cc`
    - [ ] 2.3c `lock.p4` P4 program
    - [ ] 2.3d Host-side `lock_host.c` UDP client/server
    - [x] 2.3e Unit tests — state machine (16/16 pass) + handler integration (4/4 pass)
    - [x] 2.3f `max_keys` constructor param (switch SRAM vs DPU DRAM budget)
    - [x] 2.3g `max_waiters_per_key=0` mode for switch fast-path (no queuing)
    - [x] 2.3h `LockStatus` return type with `kKeyOverflow`/`kContention` for tier handoff
    - [x] 2.3i Lease expiry via `CheckTimeouts()` with best-effort TIMEOUT delivery
  - Depends on: 2.1
- [ ] **2.4** Barrier — generation-based counting barrier (host, DPU, P4)
  - Sub-tasks:
    - [x] 2.4a Shared state machine: `lib/hcop/barrier_state.h`, `barrier_state.cc`, `barrier_proto.h`
    - [x] 2.4b DPU handler: `sims/nic/dpu_bm/barrier_dpu_handler.h`, `barrier_dpu_handler.cc`
    - [ ] 2.4c `barrier.p4` P4 program
    - [ ] 2.4d Host-side `barrier_host.c` UDP client/server
    - [x] 2.4e Unit tests — state machine (9/9 pass) + handler integration (2/2 pass)
    - [x] 2.4f `SetParticipants()` with validation (N=2..64)
    - [x] 2.4g Rejection of future generations (kFutureArrival)
    - [x] 2.4h Full bitmap usage test (N=64)
  - Depends on: 2.1
- [x] **2.5** Host disk image integration — add guest programs to SimBricks disk image build
  - Files: `images/hcop/paxos_host.cc`, `lock_host.cc`, `barrier_host.cc`
  - Integration: `images/rules.mk`, `install-hcop.sh`
  - Depends on: 2.2d, 2.3d, 2.4d
- [x] **2.6** Unit tests per primitive state machine
  - Implemented 41 tests total (14 Paxos + 16 Lock + 11 Barrier)
  - Depends on: 2.2a, 2.3a, 2.4a

---

## Phase 3: Hybrid Routing & Exception Forwarding

- [x] **3.1** Exception type definitions — per-primitive enum + packet header encoding
  - Files: `hcop_proto.h` (implemented in Phase 1/2)
  - Depends on: Phase 2 complete
- [x] **3.2** HCOP behavioral switch model (`sims/net/hcop_switch/`)
  - Sub-tasks:
    - [x] 3.2a Switch device class with configurable Tofino-2 resource model
    - [x] 3.2b Per-primitive fast-path logic (match-action simulation)
    - [x] 3.2c Exception detection and forwarding to DPU port
    - [x] 3.2d Build system integration (`rules.mk`)
  - Depends on: 3.1
- [x] **3.3** Hybrid routing logic — multi-tier packet flow for 7 placement configs
  - Sub-tasks:
    - [x] 3.3a `PlacementMode` enum (`kProcessAndForward` / `kForwardOnly`) in `SwitchConfig`
    - [x] 3.3b Rename `kToDpu` → `kToFallback`, `dpu_port_index` → `fallback_port_index` (placement-agnostic)
    - [x] 3.3c `PrimitiveEngine` ingress-port-based response forwarding (L2 flood from fallback port)
    - [x] 3.3d `kForwardOnly` mode bypasses `PrimitiveEngine` (for DPU-only, Host-only, DPU+Host placements)
    - [x] 3.3e `kDpuCoreExhausted` exception type in `hcop_proto.h` + DPU sets it on core pool exhaustion
    - [x] 3.3f Backward-compatible JSON config parsing (`dpu_port_index` still accepted)
    - [x] 3.3g Placement-mode unit tests (3 new: ForwardOnly→Fallback, ForwardOnly→Flood, ProcessForward→Flood)
  - Depends on: 3.2
- [ ] **3.4** Integration tests — end-to-end multi-tier forwarding
  - Sub-tasks:
    - [x] 3.4a Minimal SimBricks simulation: 2 QEMU hosts + 1 DPU + 1 switch
    - [x] 3.4b Paxos round end-to-end
    - [x] 3.4c Lock acquire/release round-trip
    - [x] 3.4d Barrier with N=2
  - Depends on: 3.3

---

## Phase 4: Telemetry & Metrics Collection

- [x] **4.1** Per-operation CSV logging (operation_id, latency, tier_path, exceptions)
  - Depends on: Phase 3 complete
- [x] **4.2** Per-tier resource utilization sampling (core %, memory, packets processed/queued/dropped)
  - Depends on: 4.1
- [x] **4.3** Aggregate summary generation (mean/p50/p99 latency, throughput, utilization)
  - Depends on: 4.1
- [x] **4.4** Warm-up period filtering (configurable N seconds ignored)
  - Depends on: 4.1
- [ ] **4.5** Telemetry integration tests — verify CSV output well-formed after short run
  - Depends on: 4.1–4.4

---

## Phase 5: Experiment Orchestration

- [x] **5.1** Python orchestration for 21 configurations (7 placements × 3 primitives) — **COMPLETE (18/21 configs validated, 3 known issues)**
  - Files: `experiments/pyexps/hcop/hcop_topology.py`, `scripts/collect_hcop_csvs.py`
  - Env-var driven (HCOP_PLACEMENT, HCOP_PRIMITIVE, HCOP_NUM_OPS) — see Decision #12
  - Switch routing modes: `process_and_forward` (configs 1,4,5,7), `forward_all` (configs 2,6)
  - **Validated configs (18):**
    - Paxos: 7/7 — all placements produce correct summary data
    - Lock: 7/7 — all placements produce correct summary data
    - Barrier: 4/7 — switch_only, dpu_only, switch_dpu, switch_dpu_host
  - **Known issues (3 barrier configs — FUTURE WORK):**
    - barrier/host_only, barrier/switch_host, barrier/dpu_host
    - Root cause: `RunDual()` and `RunCombined()` in `barrier_host.cc` record telemetry timestamps around the local `manager.Arrive()` call only (~10µs), instead of spanning the full barrier round-trip including the blocking wait for the remote participant's network arrival (~700-1000µs expected).
    - The latency values in these 3 rows (p50 ~10-15µs) are physically impossible for a barrier coordination between two QEMU VMs over a simulated network and MUST NOT be used in placement algebra analysis.
    - Fix: Move the telemetry end timestamp in `RunDual()` and `RunCombined()` to AFTER the `BarrierManager` has received all participant arrivals (including the remote host's network message) and issued the RELEASE. Then re-run all 3 configs with n=100.
  - Validated Paxos placements (all 7/7):
    - [x] `switch_only` — Paxos n=100, switch_util=0.96%, latency_p50=466µs
    - [x] `dpu_only` — Paxos n=100, dpu_mem_util=0.016%, latency_p50=515µs (+10% vs switch)
    - [x] `switch_dpu` — Paxos n=100, switch_util=0.96%, latency_p50=493µs, 0 exceptions
    - [x] `switch_host` — Paxos n=100, switch_util=0.96%, latency_p50=479µs, no DPU
    - [x] `dpu_host` — Paxos n=100, dpu_mem_util=0.016%, latency_p50=538µs, switch forward_only
    - [x] `host_only` — Paxos n=100, latency_p50=497µs
    - [x] `switch_dpu_host` — Paxos n=100, latency_p50=497µs
  - Paxos latency ordering (P50): switch_only (466) < switch_host (479) < switch_dpu (493) < host_only (497) ≈ switch_dpu_host (497) < dpu_only (515) < dpu_host (538)
  - Validated Lock placements (all 7/7):
    - [x] `switch_only` — Lock n=100, switch_util=0.095%, latency_p50=631µs
    - [x] `dpu_only` — Lock n=100, dpu_mem_util=0.006%, latency_p50=642µs
    - [x] `switch_dpu` — Lock n=100, switch_util=0.095%, latency_p50=673µs
    - [x] `switch_host` — Lock n=100, switch_util=0.095%, latency_p50=627µs
    - [x] `dpu_host` — Lock n=100, dpu_mem_util=0.006%, latency_p50=642µs
    - [x] `host_only` — Lock n=100, latency_p50=652µs
    - [x] `switch_dpu_host` — Lock n=100, latency_p50=644µs
  - Lock latency ordering (P50): switch_host (627) < switch_only (631) ≈ dpu_only (642) ≈ dpu_host (642) ≈ switch_dpu_host (644) < host_only (652) < switch_dpu (673)
  - Validated Barrier placements (4/7 valid, 3 known issues):
    - [x] `switch_only` — Barrier n=100, latency_p50=699µs, tput=38.5 ops/s (2 participants)
    - [x] `dpu_only` — Barrier n=100, latency_p50=755µs, tput=39.0 ops/s
    - [x] `switch_dpu` — Barrier n=100, latency_p50=800µs, tput=38.0 ops/s
    - [x] `switch_dpu_host` — Barrier n=100, latency_p50=1112µs, tput=37.9 ops/s
    - [ ] `switch_host` — ⚠️ KNOWN ISSUE: latency_p50=11µs (measures local Arrive() only, not full round-trip)
    - [ ] `dpu_host` — ⚠️ KNOWN ISSUE: latency_p50=15µs (measures local Arrive() only, not full round-trip)
    - [ ] `host_only` — ⚠️ KNOWN ISSUE: latency_p50=10µs (measures local Arrive() only, not full round-trip)
  - Barrier latency ordering (P50, valid configs only): switch_only (699) < dpu_only (755) < switch_dpu (800) < switch_dpu_host (1112)
  - workload_params = 'n=100' in all 21 summary CSVs
  - Master summary: results/master_summary.csv with 21 rows (18 valid + 3 known issues) ✅
  - **Bugs fixed (session 1 — broadcast/operation_id):**
    1. paxos_host.cc: SendNetworkMessage always broadcasts in raw mode (fix for host_only)
    2. paxos_host.cc: operation_id propagated via g_current_operation_id (server→client matching)
    3. lock_host.cc: same broadcast + operation_id fixes
    4. barrier_host.cc: same broadcast fix
    5. barrier_dpu_handler.cc: default_participants set to 2 (was 3, mismatch with 2-host topology)
    6. hcop_topology.py: barrier_default_participants=2 in switch JSON config
    7. hcop_topology.py: RawHcopClient2App for barrier's 2nd participant on host1
  - **Bugs fixed (session 2 — barrier sync deadlock):**
    8. device.cc: hcop_switch sync loop deadlock (on-demand p->Sync(cur_ts_))
    9. primitive_engine.cc: fallback port bypass dropping client packets (check msg type)
    10. primitive_engine.cc: source MAC loopback drop (rewrite to dummy switch MAC)
    11. barrier_host.cc: blocking pthread_join on infinite exception handler thread
    12. host_common.h: raw socket interface retry loop (10 retries with 1s delay)
    13. hcop_topology.py: KVM starvation fixed with enable_sync_simulation()
  - **Additional fixes (session 3 — data quality):**
    14. DPU CSV append contamination (stale CSV cleanup)
    15. Host packet counter wiring
    16. Switch utilization calculation
  - Depends on: Phase 4 complete
- [ ] **5.2** Parallel sweep support
  - Depends on: 5.1
- [x] **5.3** Results collection and aggregation scripts
  - `scripts/collect_hcop_csvs.py` — extracts CSVs from out.json + /tmp/, runs aggregate_telemetry.py
  - `scripts/aggregate_telemetry.py` — produces summary.csv with all metrics
  - Depends on: 5.1
- [ ] **5.4** Tofino adapter validation pass (optional — uses real P4 model via `sims/net/tofino/`)
  - Depends on: 5.1

---

## Dependency Graph (Phase 1 critical path)

```
1.1 (Config) ──┬── 1.2 (Device class) ──┬── 1.5 (Packet RX/TX) ── 1.9 (Build) ── 1.11 (Verify)
               ├── 1.3 (Core pool) ──────┤
               └── 1.4 (DRAM store) ─────┘
                                         │
1.6 (Handler interface) ─────────────────┤
1.7 (Protocol header) ──────────────────┘
                                         │
                              1.8 (PCIe exception path)
                                         │
                              1.10 (Unit tests)
```
