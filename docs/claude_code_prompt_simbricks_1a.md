# Claude Code Prompt: SimBricks Experiment 1A Build-Out
#prompts

Build the DPU behavioral model, coordination primitive implementations, hybrid routing logic, telemetry instrumentation, and experiment orchestration for Experiment 1A (Placement Space Exploration) of the HCOP thrust. This is a 3-tier heterogeneous placement experiment (Tofino-2 switch → BlueField-3 DPU → host CPU) testing three coordination primitives (Paxos consensus, distributed locking, barrier synchronization) across 7 placement configurations per primitive.

## My engineering preferences (use these to guide your recommendations):
- DRY is important—flag repetition aggressively.
- Well-tested code is non-negotiable; I'd rather have too many tests than too few.
- I want code that's "engineered enough" — not under-engineered (fragile, hacky) and not over-engineered (premature abstraction, unnecessary complexity).
- I err on the side of handling more edge cases, not fewer; thoughtfulness > speed.
- Bias toward explicit over clever.
- C++ for the DPU behavioral model and anything touching SimBricks internals (match the existing codebase style in `sims/nic/` and `lib/simbricks/nicbm`).
- P4-16 for switch-tier logic, targeting Tofino-2 via the Barefoot SDE compiler.
- Python for orchestration scripts (match the existing `experiments/pyexps/` style).
- Parameterize everything that maps to a hardware capability vector — no magic numbers baked into models.

## Project-specific context you MUST understand before writing code:
- Read `lib/simbricks/nicbm/nicbm.h` and at least one existing behavioral model (e.g., `sims/nic/i40e_bm/`) to understand the base class, PCIe transaction handling, and Ethernet packet flow.
- Read `sims/net/tofino/` to understand how the Tofino adapter bridges SimBricks ↔ Tofino SDK simulator.
- Read `experiments/simbricks/orchestration/` to understand how experiments define topologies (hosts, NICs, networks, connections).
- Read `sims/net/net_switch/` for the behavioral switch model pattern.
- The DPU model is a PCIe device (like a NIC) that also connects to the network fabric. It receives packets, processes coordination logic with configurable latency, manages state in simulated DRAM, and forwards exceptions to the host or switch as needed.
- Hybrid placements require packets to cross tier boundaries. The switch fast-path handles common cases at line rate; exceptions get forwarded to the DPU or host. The crossing cost (S→D, D→H, S→H) is modeled by SimBricks' PCIe/network protocols but we also need to track it in telemetry.

## Build order (phases):
### Phase 1: DPU Behavioral Model (`sims/nic/dpu_bm/`)
### Phase 2: Coordination Primitives (host + P4 + DPU implementations)
### Phase 3: Hybrid Routing & Exception Forwarding
### Phase 4: Telemetry & Metrics Collection
### Phase 5: Experiment Orchestration Scripts

---

## 1. Architecture review
Evaluate (per phase):
- Component boundaries: DPU model internal architecture (packet RX path → dispatch → processing pipeline → state store → TX path). How does it compose with SimBricks' existing NIC model base class?
- Coupling: DPU model should depend ONLY on `lib/simbricks/nicbm` and `lib/simbricks/network`. No dependencies on specific coordination primitive logic — primitives register as handlers.
- Data flow: trace a Paxos accept message from client (QEMU guest) → switch (Tofino adapter, fast path) → DPU (slow path, exception) → back to switch → back to client. Identify every SimBricks protocol boundary crossed and verify the model handles each transition.
- Scaling: the model must support N concurrent primitives sharing DPU resources (ARM core pool, DRAM budget). Verify the resource contention model doesn't serialize everything through a single lock.
- Configuration: every hardware capability vector field from Step 1 (see below) must be a runtime parameter, not a compile-time constant. Use a JSON or YAML config file loaded at startup.

### Hardware capability vector fields the DPU model MUST parameterize:
```
arm_cores: 16                    # concurrent processing slots
dram_capacity_mb: 16384          # state storage limit
pcie_gen: 5                      # affects host crossing latency
pcie_lanes: 16
per_packet_base_latency_ns: TBD  # from DOCA benchmarks
crypto_accel: true               # enables fast-path crypto ops
doca_flow_steering: true         # hardware flow classification
service_rate_pps: TBD            # max packets/sec sustained
```

### Hardware capability vector fields for the Switch (Tofino-2) — used by P4 programs and orchestration:
```
pipelines: 4
stages_per_pipeline: 20
sram_pages_per_pipeline: 1600    # 16 KiB each → ~25 MiB/pipeline
tcam_blocks_per_pipeline: 480
alu_model: integer_only          # add/sub/compare/shift/bitwise, no FP
packet_buffer_mb: 64
recirculation_supported: true
```

### Hardware capability vector fields for the Host — used by orchestration:
```
model: general_purpose_x86
cores: configurable
memory: configurable
processing_latency_ns: TBD       # baseline kernel networking overhead
programming_model: unrestricted
```

## 2. Code quality review
Evaluate:
- Code organization: one directory per component (`sims/nic/dpu_bm/`, `sims/net/p4_primitives/`, `experiments/pyexps/hcop/`). Shared types in a common header.
- DRY violations: the three coordination primitives share common patterns (receive msg → lookup state → compute → update state → send reply). Extract a `PrimitiveHandler` base class; don't copy-paste the dispatch loop three times.
- Naming: match SimBricks conventions. Look at `i40e_bm` for naming patterns in the NIC models.
- Error handling: the DPU model MUST handle gracefully: packets for unregistered primitives, DRAM exhaustion (state overflow), ARM core pool exhaustion (all cores busy), malformed packets. Log warnings, don't crash the simulation.
- Edge cases for hybrid placements: what happens when the switch forwards an exception to the DPU but the DPU's processing queue is full? What happens when a DPU-to-host overflow occurs mid-Paxos-round? Define and handle these explicitly.
- Makefile integration: new components must integrate with SimBricks' top-level `Makefile`. Follow the pattern used by `sims/nic/i40e_bm/Makefile`.

## 3. Test review
Evaluate:
- Unit tests for the DPU behavioral model: test packet dispatch, state management, resource accounting (core allocation/release, DRAM tracking), configuration loading.
- Unit tests for each coordination primitive: test the protocol state machine in isolation (not through SimBricks). Paxos: test prepare/promise/accept/learn sequences, leader election, conflicting proposals. Locking: test grant/release, contention queuing, timeout/expiry. Barrier: test arrival counting, release notification, late arrivals.
- Integration tests: run a minimal SimBricks simulation with 2 QEMU hosts + 1 DPU model + 1 switch model. Verify a single Paxos round completes end-to-end. Verify a lock acquire/release round-trip. Verify a barrier with N=2.
- Configuration tests: verify the model rejects invalid configs (negative core count, zero DRAM, etc.).
- Telemetry tests: verify that CSV output files are well-formed and contain expected columns after a short simulation run.
- Missing edge case coverage: what happens under resource exhaustion? Under packet loss (if ns-3 is configured to drop)? Under reordering?

## 4. Performance review
Evaluate:
- Simulation speed: the DPU behavioral model should not be the bottleneck. Profile packet processing latency in the model itself. If per-packet overhead exceeds 1µs of real wall-clock time, something is wrong.
- Memory usage: if the model tracks per-flow state for millions of simulated flows, verify it doesn't OOM the simulation host. Use bounded data structures where possible.
- Orchestration efficiency: experiment scripts that sweep 21 configurations (7 placements × 3 primitives) should support parallel execution where SimBricks allows.
- Telemetry overhead: CSV logging should buffer writes, not flush per-packet.
- Simulation warm-up: ensure metrics collection ignores the first N seconds of simulation to avoid cold-start artifacts.

---

## For each issue you find:
For every specific issue (bug, smell, design concern, or risk):
- Describe the problem concretely, with file and line references.
- Present 2–3 options, including "do nothing" where that's reasonable.
- For each option, specify: implementation effort, risk, impact on other code, and maintenance burden.
- Give me your recommended option and why, mapped to my preferences above.
- Then explicitly ask whether I agree or want to choose a different direction before proceeding.

## Workflow and interaction:
- Do not assume my priorities on timeline or scale.
- After each section, pause and ask for my feedback before moving on.
- When you need to look at SimBricks source to understand a pattern, do it — don't guess at the API.

## BEFORE YOU START:
Ask if I want one of two options:
1) BIG BUILD: Work through this phase at a time (Phase 1: DPU Model → Phase 2: Primitives → Phase 3: Hybrids → Phase 4: Telemetry → Phase 5: Orchestration) with architecture + code quality + test + performance review at each phase before moving to the next. At most 4 top issues per review stage.
2) COMPONENT BUILD: Work through interactively ONE component at a time within a phase (e.g., just the DPU packet dispatch loop, or just the Paxos P4 program).

FOR EACH PHASE: output the implementation plan and tradeoffs, your opinionated recommendation and why, and then use AskUserQuestion. Also NUMBER issues and then give LETTERS for options so each option clearly labels the issue NUMBER and option LETTER so the user doesn't get confused. Make the recommended option always the 1st option.

---

## Reference: The 21 Experiment Configurations (Step 3 Matrix)

For each primitive (Paxos, Locking, Barrier), test these 7 placements:

| # | Placement | What runs where |
|---|-----------|----------------|
| 1 | Switch-only | All logic in P4 on Tofino-2 |
| 2 | DPU-only | All logic in DPU behavioral model |
| 3 | Host-only | All logic in QEMU guest (baseline) |
| 4 | Switch+DPU | Fast path on switch, exceptions/overflow to DPU |
| 5 | Switch+Host | Fast path on switch, exceptions/overflow to host |
| 6 | DPU+Host | Primary on DPU, overflow/recovery to host |
| 7 | Switch+DPU+Host | Full 3-tier: switch fast path → DPU slow path → host recovery |

## Reference: Metrics to collect per experiment run (Step 4)

```
# Per-operation metrics (one row per completed operation)
operation_id, primitive_type, placement_config, start_time_ns, end_time_ns,
latency_ns, tier_path (e.g., "S→D→H"), num_tier_crossings,
was_exception (bool), was_overflow (bool)

# Per-tier resource utilization (sampled every T ms)
timestamp_ns, tier, core_utilization_pct, memory_used_bytes,
memory_capacity_bytes, packets_processed, packets_queued,
packets_dropped

# Aggregate summary (one row per experiment run)
placement_config, primitive_type, workload_params,
latency_mean_ns, latency_p50_ns, latency_p99_ns,
throughput_ops_per_sec, host_cpu_utilization_pct,
switch_stage_utilization_pct, dpu_core_utilization_pct,
dpu_memory_utilization_pct, exception_rate_pct, overflow_rate_pct
```

## Reference: Key SimBricks files to read first

```
lib/simbricks/nicbm/nicbm.h          # Base class for behavioral NIC/device models
sims/nic/i40e_bm/                     # Example behavioral NIC model (pattern to follow)
sims/nic/corundum_bm/                 # Another behavioral model example
sims/net/net_switch/switch.cc         # Behavioral switch model
sims/net/tofino/                      # Tofino simulator adapter
lib/simbricks/network/               # Network protocol (Ethernet between components)
lib/simbricks/pcie/                   # PCIe protocol (host ↔ device)
experiments/simbricks/orchestration/  # Python experiment framework
experiments/pyexps/                   # Example experiment scripts
```
## Required reading before each phase:
- Read `docs/hcop_1a_decisions.md` for all resolved design decisions.
- Read `docs/hcop_1a_plan.md` for current implementation status and next tasks.
- Do NOT modify CLAUDE.md. Update the plan file as you complete tasks.

## CRITICAL: Disk Image Build
Packer is broken on this machine. NEVER use `make images/output-hcop/hcop`.
Use `docs/rebuild_hcop_image.sh` instead (qemu-nbd direct mount approach).