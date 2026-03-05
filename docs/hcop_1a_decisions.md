# Design Decisions — Experiment 1A

### 1. DPU Model: Dual Interface

**Answer: Model it as a NIC (PCIe to host, single Ethernet to switch).** This matches the real BlueField-3 topology. In production, the DPU sits on the host's PCIe bus and its uplink connects to the switch — it's not a bump-in-the-wire with two Ethernet interfaces. The `i40e_bm` NIC pattern is architecturally correct here.

For hybrid placements where the switch forwards an exception to the DPU, those packets arrive on the DPU's Ethernet port from the switch (just like any other ingress packet from the network). The DPU processes them and either responds back through Ethernet (to the switch/network) or escalates to the host via PCIe DMA. This is exactly what the existing `nicbm` base class supports.

The one subtlety from your v5 document: "L(S→H) is not necessarily equal to L(S→D) + L(D→H). Record the physical topology (direct NIC vs DPU NIC forwarding) and treat S→H as a separate edge term." So for the S→H crossing cost in placements that bypass the DPU, you'll want the orchestration to be able to configure whether traffic goes switch→DPU→host (PCIe path through the DPU) or switch→host-NIC→host (separate NIC, bypassing DPU). For Phase 1, I'd say start with DPU always in-path (the realistic BlueField-3 deployment topology) and add the bypass path as a configuration option later.

### 2. Tofino Adapter Dependency

**Answer: I have Open P4 Studio installed at ~/open-p4studio. Use it for two things: (a) compile all P4 programs and extract resource usage reports from the compiler output to calibrate the behavioral switch model, and (b) run validation experiments through the real SimBricks Tofino adapter (sims/net/tofino/). For the bulk of experiment sweeps, use the behavioral hcop_switch model parameterized from the compiler reports. Build the behavioral model first (it's the fast iteration path), then add Tofino adapter integration as a validation pass.

### 3. Host-Tier Coordination Logic

**Answer: Write the guest-side client/server programs from scratch, using kernel UDP sockets.** SimBricks' disk images include a base Linux image. You'll need lightweight programs that implement each primitive's protocol — these are not complex applications. Standard kernel UDP is the right transport for the host-only baseline because it captures the real penalty of kernel networking (context switches, socket buffer copies, scheduling jitter) which is exactly what the host latency penalty `L_H_sched` models.

Don't use DPDK for the host baseline — it bypasses the penalties that justify offloading to DPU/switch in the first place. If you want a DPDK comparison later, add it as a separate "optimized host" configuration, but the default host baseline should reflect what a typical application developer would write.

The programs should be simple: a client that issues operations (Paxos proposals, lock acquire/release, barrier arrive) and measures end-to-end latency, and a server that implements the protocol state machine. Shared library for the protocol logic so the same state machine code can be used across host-only, DPU+Host, and Switch+Host placements.

### 4. TBD Values & Calibration

**Answer: Use reasonable defaults from public specs; flag them clearly for later calibration.**

Proposed defaults:
- `per_packet_base_latency_ns`: **2000 ns (2 µs)** for the DPU ARM path. This is in the range of published DOCA Flow steering benchmarks for simple forwarding operations. More complex processing (crypto, state lookup) adds on top.
- `service_rate_pps`: **40 Mpps** aggregate across all ARM cores. Conservative for BlueField-3 (published numbers go higher but depend heavily on packet size and operation complexity).
- `host_processing_latency_ns`: **15000 ns (15 µs)** for kernel UDP round-trip overhead. This is a reasonable mid-range value for standard kernel networking without DPDK, including socket buffer copies and scheduling.

Mark all three with a `CALIBRATION_PLACEHOLDER` comment in the config so they're easy to find and replace once you have real hardware measurements.

### 5. Paxos Variant

**Answer: Classic Multi-Paxos with a stable leader, 3 replicas default.** This matches P4xos (which your prior art audit identifies as the closest comparator) and is the most natural fit for the switch fast-path pattern: the steady-state accept/learn path is simple enough for P4, while leader election is the exception that overflows to DPU/host.

The stable leader assumption means the common case (Phase 2a → accept) is a 2-message exchange that fits in switch register operations. Leader election (Phase 1 prepare/promise) triggers infrequently and constitutes the "exception" for hybrid placements.

Make replica count configurable (3 default, test with 5 for scaling).

### 6. Locking Semantics

**Answer: Simple mutual-exclusion, per-key, with configurable lease duration (default 10ms).** This maps cleanly to the FissLock decomposition template from your prior art audit: the switch handles the fast-path grant decision (check-and-set on a per-key register), contention queuing overflows to DPU, and the host handles recovery/timeout.

Start with per-key because it's more realistic and exercises the switch SRAM capacity limits (you'll hit the state overflow boundary naturally as key count grows, which triggers exceptions — great for validating the placement algebra). Reader-writer locks add complexity without adding much to the placement algebra validation; defer to Phase 2 if interesting.

### 7. Barrier Semantics

**Answer: Simple counting barrier, reusable (generation-based), N=2 to 64.** A generation counter lets you reuse barriers across rounds without reset races. The switch implementation is natural: a P4 register counts arrivals per generation and broadcasts the release notification when count hits N.

N=2 is your minimal integration test. N=8–16 exercises the switch multicast resources. N=64 pushes toward switch state limits and exercises the DPU overflow path. Keep N configurable in the experiment sweep.

### 8. Exception Criteria

**Answer: Define concrete exception types per primitive, not a generic bit.** This is important for the telemetry — you need to know *why* something overflowed, not just that it did. The placement algebra's value comes from predicting which dimension causes the overflow.

Proposed exception types:
- **Paxos:** `LEADER_ELECTION` (non-steady-state), `STATE_OVERFLOW` (instance log exceeds switch SRAM), `CONFLICT` (concurrent proposals needing arbitration)
- **Locking:** `CONTENTION` (key already held, needs queuing), `STATE_OVERFLOW` (too many tracked keys for switch SRAM), `TIMEOUT_CHECK` (lease expiry verification)
- **Barrier:** `GENERATION_OVERFLOW` (too many concurrent barriers), `LATE_ARRIVAL` (arrived after release, needs recovery)

Encode the exception type in a packet header field (e.g., a custom EtherType or a reserved field in your protocol header). This way telemetry can break down exception rates by type per primitive — critical for the placement algebra's predictive power.

### 9. Switch→DPU Forwarding Mechanism

**Answer: Switch sends exception packets directly to the DPU's Ethernet port.** The DPU has a dedicated switch port in the topology. This is the realistic physical topology: the BlueField-3's uplink connects to a switch port. In SimBricks, this means the DPU model's Ethernet interface connects to one of the switch model's ports through ns-3 (or directly via a wire component).

Don't route exceptions through the host PCIe path first — that adds unnecessary latency and doesn't match real deployment. The switch identifies an exception, sets the exception type header, and forwards the packet out the port connected to the DPU. The DPU processes it and sends a response back through the same Ethernet port.

### 10. Build Mode

**My recommendation: BIG BUILD.** Given the dependency chain (DPU model must exist before primitives can be tested on it, primitives must exist before hybrids, etc.), phase-by-phase with checkpoint reviews makes the most sense. Component-level granularity would be useful within Phase 1 (the DPU model has enough internal complexity), but tell Claude Code to use BIG BUILD mode and break Phase 1 into sub-components during that phase's review.

### 11. Orchestration Framework Version

**Answer: Use the newer `simbricks.orchestration` package** (with `system`, `simulation`, `instantiation` modules) that `simple_demo.py` uses. The prompt's reference to `experiments/simbricks/orchestration/` is outdated — Claude Code correctly identified the discrepancy. Follow the pattern from the actual current codebase, not the prompt's file paths.

### 12. copy_instantiation JSON Round-Trip (Workaround)

**Problem:** `simbricks-run` calls `copy_instantiation()` in `__main__.py:247` which does a full `toJSON()` → `fromJSON()` round-trip on the simulation before executing it. Any custom instance attributes added to Simulator or Application subclasses (e.g., `placement_mode`, `primitive`, `operations`) are **silently lost** during this serialization because `toJSON()`/`fromJSON()` in the base classes don't know about them.

This caused `AttributeError: 'HCOPSwitchSim' object has no attribute 'placement_mode'` when the `prepare()` method tried to read the attribute at runtime.

**Workaround:** All runtime configuration in `hcop_topology.py` is read from **environment variables** (`HCOP_PLACEMENT`, `HCOP_PRIMITIVE`, `HCOP_NUM_OPS`) inside `prepare()` and `run_cmds()` methods, rather than stored as instance attributes. Environment variables survive the serialization round-trip because they are part of the process environment, not the Python object graph.

**Long-term fix:** Either (a) override `toJSON()`/`fromJSON()` in subclasses to serialize custom attributes, or (b) use the `_parameters` dict on Fragment (which IS serialized) to pass configuration. The env-var approach is pragmatic for Phase 1 where we control the entire pipeline.

### 13. Placement Routing Architecture

**Audit result:** The switch `PrimitiveEngine` already had `kProcessAndForward` and `kForwardOnly` placement modes (implemented in Phase 3, Task 3.3). The `SwitchConfig` accepts `"placement_mode": "forward_all"` from JSON and routes packets accordingly:

- `kProcessAndForward`: Runs HCOP state machines, forwards exceptions to `fallback_port_index`
- `kForwardOnly`: Forwards all HCOP packets to `fallback_port_index` without processing; L2-floods responses back

The `fallback_port_index` is set per-placement:
- `switch_only`: -1 (no fallback — switch is sole processor)
- `dpu_only`, `switch_dpu`, `dpu_host`, `switch_dpu_host`: 2 (DPU port)
- `switch_host`: 1 (host1 NIC port — host1 runs server)

**DPU passthrough NOT needed.** For config #5 (`switch_host`), the DPU is simply omitted from the topology. The switch's `fallback_port_index` points to host1's NIC port directly. This avoids adding complexity to the DPU model and is architecturally cleaner.

### 14. Phase 5.1 Closure — 18/21 Acceptance

Accepted 18 of 21 experiment configs as validated. The 3 remaining barrier configs (host_only, switch_host, dpu_host) have a known telemetry timing bug where `RunDual()`/`RunCombined()` measures local function call latency (~10µs) instead of full barrier round-trip latency (~700-1000µs expected).

Rationale for proceeding: Paxos (7/7) and Lock (7/7) provide complete placement comparison data across all 7 configs. Barrier has 4 valid configs spanning single-tier (switch_only, dpu_only) and multi-tier (switch_dpu, switch_dpu_host) placements. The 18 valid data points are sufficient for initial placement algebra formalization. The 3 broken barrier configs will be fixed in a future session as part of the barrier timing remediation work.