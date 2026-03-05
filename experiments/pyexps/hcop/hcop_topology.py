import sys
import os
import json
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from simbricks.orchestration import instantiation as inst
from simbricks.orchestration import simulation as sim
from simbricks.orchestration import system
from simbricks.orchestration.helpers import instantiation as inst_helpers
from simbricks.orchestration.helpers import simulation as sim_helpers
from simbricks.orchestration.system import nic as sys_nic
from simbricks.orchestration.simulation import pcidev as sim_pcidev
from simbricks.orchestration.simulation.net import net_base as sim_net
from simbricks.orchestration.instantiation import base as inst_base
from simbricks.orchestration.system.host import app

# ╔═══════════════════════════════════════════════════════════════════╗
# ║  Simulator Wrappers                                              ║
# ╚═══════════════════════════════════════════════════════════════════╝

class DpuBMNIC(sys_nic.SimplePCIeNIC):
    def __init__(self, s: system.System) -> None:
        super().__init__(s)

class DpuBMSim(sim_pcidev.NICSim):
    def __init__(self, simulation: sim.Simulation):
        super().__init__(
            simulation=simulation,
            executable="sims/nic/dpu_bm/dpu_bm",
        )
        self.name = f"DpuBMSim-{self._id}"

    async def prepare(self, inst: inst_base.Instantiation):
        await super().prepare(inst)
        config = {
            "cores": 4,
            "dram_capacity": 4294967296,
            "mac_address": "00:11:22:33:44:03",
            "telemetry_interval_ms": 10
        }
        config_path = f"{inst.env.get_simulator_output_dir(self)}/dpu_config.json"
        os.makedirs(os.path.dirname(config_path), exist_ok=True)
        with open(config_path, 'w') as f:
            json.dump(config, f)

    def run_cmd(self, inst: inst_base.Instantiation) -> str:
        base_cmd = super().run_cmd(inst)
        config_path = f"{inst.env.get_simulator_output_dir(self)}/dpu_config.json"
        return f"{base_cmd} -c {config_path}"

class HCOPSwitch(system.EthSwitch):
    pass

class HCOPSwitchSim(sim_net.SwitchNet):
    def __init__(self, simulation: sim.Simulation):
        super().__init__(
            simulation=simulation,
            executable="sims/net/hcop_switch/hcop_switch",
        )
        self.name = f"HCOPSwitchSim-{self._id}"

    async def prepare(self, inst: inst_base.Instantiation):
        await super().prepare(inst)
        placement_config = os.environ.get("HCOP_PLACEMENT", "switch_only")

        # ---- Placement mode ----
        # forward_only: switch does NOT run PrimitiveEngine, just forwards
        # process_and_forward: switch processes HCOP packets locally
        if placement_config in ["dpu_only", "dpu_host"]:
            switch_mode = "forward_only"
        else:
            switch_mode = "process_and_forward"

        # ---- Fallback port index ----
        # Port 0 = host0 NIC, Port 1 = host1 NIC, Port 2 = DPU NIC (when present)
        #
        # Placements with DPU as fallback: switch_dpu, switch_dpu_host, dpu_only, dpu_host
        #   -> fallback_port_index = 2 (DPU port)
        # Placements with host as fallback: switch_host
        #   -> fallback_port_index = 1 (host1 NIC port, host1 runs server)
        # Placements with no fallback: switch_only
        #   -> fallback_port_index = -1 (no fallback)
        if placement_config in ["dpu_host", "switch_dpu_host", "switch_host"]:
            fallback_port = 1
        elif placement_config in ["dpu_only", "switch_dpu"]:
            fallback_port = 2
        else:
            fallback_port = -1  # switch_only: no fallback

        config = {
            "sram_pages_total": 1024,
            "tcam_blocks_total": 64,
            "placement_mode": switch_mode,
            "num_replicas": 3,
            "switch_node_id": 2,
            "barrier_default_participants": 2,
            "telemetry_interval_ms": 10,
            "topology": {
               "fallback_port_index": fallback_port,
               "nodes": [
                   {"id": 0, "port": 0},
                   {"id": 1, "port": 1}
               ]
            }
        }
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        os.makedirs(os.path.dirname(config_path), exist_ok=True)
        with open(config_path, 'w') as f:
            json.dump(config, f)

    def run_cmd(self, inst: inst_base.Instantiation) -> str:
        base_cmd = super().run_cmd(inst)
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        return f"{base_cmd} -c {config_path}"

# ╔═══════════════════════════════════════════════════════════════════╗
# ║  Host Applications                                               ║
# ╚═══════════════════════════════════════════════════════════════════╝

class RawHcopClientApp(app.GenericRawCommandApplication):
    """Client application that sends HCOP operations (Paxos/Lock/Barrier)."""
    def __init__(self, host):
        super().__init__(host)
        self.wait = True

    def run_cmds(self, inst) -> list[str]:
        primitive = os.environ.get("HCOP_PRIMITIVE", "paxos")
        operations = os.environ.get("HCOP_NUM_OPS", "100")
        placement = os.environ.get("HCOP_PLACEMENT", "switch_only")
        # In host_only, client must target the server host (10.0.0.2).
        # In all other modes, the HCOP switch intercepts by EtherType.
        if placement == "host_only":
            target_ip = "10.0.0.2"
        else:
            target_ip = "10.0.0.1"

        delay_us = os.environ.get("HCOP_DELAY_US", "50000")
        if primitive == "paxos":
            cmd = f"paxos_host --client -i 0 -r 2 -t {target_ip} -v 42 -f eth0 -m raw -o -n {operations} -d {delay_us}"
        elif primitive == "lock":
            cmd = f"lock_host --client -i 0 -t {target_ip} -l 1 -o acquire -f eth0 -m raw -n {operations} -d {delay_us}"
        elif primitive == "barrier":
            cmd = f"barrier_host --client -i 0 -t {target_ip} -b 1 -p 2 -f eth0 -m raw -n {operations} -d {delay_us}"
        else:
            cmd = f"echo Unknown primitive: {primitive}"

        return [
            f"ls -la /usr/local/bin/{primitive}_host",
            cmd,
        ]

class RawHcopClient2App(app.GenericRawCommandApplication):
    """Second barrier participant (id=1). Used on host1 for barrier primitive."""
    def __init__(self, host):
        super().__init__(host)
        self.wait = True

    def run_cmds(self, inst) -> list[str]:
        operations = os.environ.get("HCOP_NUM_OPS", "100")
        delay_us = os.environ.get("HCOP_DELAY_US", "50000")
        target_ip = "10.0.0.1"  # target doesn't matter for raw broadcast
        cmd = f"barrier_host --client -i 1 -t {target_ip} -b 1 -p 2 -f eth0 -m raw -n {operations} -d {delay_us}"
        return [cmd]

class RawHcopServerApp(app.GenericRawCommandApplication):
    """Server for host-tier processing (used in host_only, switch_host, dpu_host, switch_dpu_host)."""
    def __init__(self, host):
        super().__init__(host)
        self.wait = True

    def run_cmds(self, inst) -> list[str]:
        primitive = os.environ.get("HCOP_PRIMITIVE", "paxos")
        operations = os.environ.get("HCOP_NUM_OPS", "100")
        if primitive == "paxos":
            cmd = f"paxos_host --server -i 1 -E {operations} --interval 10 --iface eth0 -m raw"
        elif primitive == "lock":
            cmd = f"lock_host --server -i 1 -E {operations} --interval 10 --iface eth0 -m raw"
        elif primitive == "barrier":
            cmd = f"barrier_host --server -p 2 -E {operations} --interval 10 --iface eth0 -m raw"
        else:
            cmd = f"echo Unknown primitive: {primitive}"
        return [cmd]

class BarrierCombinedApp(app.GenericRawCommandApplication):
    """Barrier combined coordinator+participant for host_only. Host1 acts as both."""
    def __init__(self, host):
        super().__init__(host)
        self.wait = True

    def run_cmds(self, inst) -> list[str]:
        operations = os.environ.get("HCOP_NUM_OPS", "100")
        delay_us = os.environ.get("HCOP_DELAY_US", "50000")
        cmd = f"barrier_host --combined -i 1 -b 1 -p 2 -f eth0 -m raw -n {operations} -d {delay_us}"
        return [cmd]

class BarrierDualApp(app.GenericRawCommandApplication):
    """Barrier dual client+exception-handler for switch_host/dpu_host. Host1 as participant + exception server."""
    def __init__(self, host):
        super().__init__(host)
        self.wait = True

    def run_cmds(self, inst) -> list[str]:
        operations = os.environ.get("HCOP_NUM_OPS", "100")
        delay_us = os.environ.get("HCOP_DELAY_US", "50000")
        target_ip = "10.0.0.1"  # target for switch-based placements
        cmd = f"barrier_host --dual -i 1 -t {target_ip} -b 1 -p 2 -f eth0 -m raw -n {operations} -d {delay_us}"
        return [cmd]

# ╔═══════════════════════════════════════════════════════════════════╗
# ║  Topology Factory                                                ║
# ║                                                                   ║
# ║  Placement routing table:                                         ║
# ║  ┌─────────────────┬──────────────────┬────────┬──────────────┐  ║
# ║  │ Placement       │ Switch           │ DPU    │ Host1        │  ║
# ║  ├─────────────────┼──────────────────┼────────┼──────────────┤  ║
# ║  │ switch_only     │ process, fb=-1   │ NO     │ server       │  ║
# ║  │ dpu_only        │ forward_all,fb=2 │ YES    │ sleep        │  ║
# ║  │ host_only       │ NO               │ NO     │ server       │  ║
# ║  │ switch_dpu      │ process, fb=2    │ YES    │ sleep        │  ║
# ║  │ switch_host     │ process, fb=1    │ NO     │ server       │  ║
# ║  │ dpu_host        │ forward_all,fb=2 │ YES    │ server       │  ║
# ║  │ switch_dpu_host │ process, fb=2    │ YES    │ server       │  ║
# ║  └─────────────────┴──────────────────┴────────┴──────────────┘  ║
# ╚═══════════════════════════════════════════════════════════════════╝

def create_topology(placement_config, primitive_type, num_operations=100):
    sys_b = system.System()
    # Abbreviate names to avoid Unix domain socket path limit (107 chars).
    # The full path includes the workdir + sim name + shm subpath.
    _short = {
        "switch_only": "sw", "dpu_only": "dp", "host_only": "ho",
        "switch_dpu": "sd", "switch_host": "sh", "dpu_host": "dh",
        "switch_dpu_host": "sdh",
    }
    _pshort = {"paxos": "px", "lock": "lk", "barrier": "br"}
    sys_b.name = f"hcop_{_pshort.get(primitive_type, primitive_type)}_{_short.get(placement_config, placement_config)}"
    distro_disk_image = system.DistroDiskImage(sys_b, "hcop")

    # ---- Determine what components are needed ----
    has_switch = placement_config != "host_only"
    has_dpu = placement_config in ["dpu_only", "switch_dpu", "dpu_host", "switch_dpu_host"]
    # Host1 runs a server when the host tier participates in processing
    host1_is_server = placement_config in [
        "host_only", "switch_host", "dpu_host", "switch_dpu_host"
    ]

    # ---- Host 0: always a client ----
    host0 = system.I40ELinuxHost(sys_b)
    host0.name = "host0"
    host0.add_disk(distro_disk_image)
    host0.add_disk(system.LinuxConfigDiskImage(sys_b, host0))
    nic0 = system.IntelI40eNIC(sys_b)
    nic0.add_ipv4("10.0.0.1")
    host0.connect_pcie_dev(nic0)
    host0.add_app(RawHcopClientApp(host0))

    # ---- Host 1: server OR passive (sleep) ----
    host1 = system.I40ELinuxHost(sys_b)
    host1.name = "host1"
    host1.add_disk(distro_disk_image)
    host1.add_disk(system.LinuxConfigDiskImage(sys_b, host1))
    nic1 = system.IntelI40eNIC(sys_b)
    nic1.add_ipv4("10.0.0.2")
    host1.connect_pcie_dev(nic1)

    if host1_is_server:
        if primitive_type == "barrier":
            # Barrier + server placement: host1 needs combined or dual mode
            if placement_config == "host_only":
                host1.add_app(BarrierCombinedApp(host1))  # coordinator + participant
            else:
                host1.add_app(BarrierDualApp(host1))  # client + exception handler
        else:
            host1.add_app(RawHcopServerApp(host1))
    else:
        # For barrier: host1 must also be a participant (client with id=1)
        # For paxos/lock: host1 just runs the server for clean shutdown
        if primitive_type == "barrier":
            host1.add_app(RawHcopClient2App(host1))
        else:
            host1.add_app(RawHcopServerApp(host1))  # Still run server for clean shutdown

    # ---- DPU (optional) ----
    if has_dpu:
        host2 = system.I40ELinuxHost(sys_b)
        host2.name = "host-dpu"
        host2.add_disk(distro_disk_image)
        host2.add_disk(system.LinuxConfigDiskImage(sys_b, host2))
        dpu_nic = DpuBMNIC(sys_b)
        dpu_nic.add_ipv4("10.0.0.3")
        host2.connect_pcie_dev(dpu_nic)
        host2.add_app(system.Sleep(host2, infinite=True))

    # ---- Component map ----
    compmap = {
        system.FullSystemHost: sim.QemuSim,
        system.IntelI40eNIC: sim.I40eNicSim,
    }

    # ---- Switch ----
    if has_switch:
        # HCOP switch for placements that use it
        switch0 = HCOPSwitch(sys_b)
        switch0.connect_eth_peer_if(nic0._eth_if)  # Port 0 = host0
        switch0.connect_eth_peer_if(nic1._eth_if)  # Port 1 = host1
        if has_dpu:
            switch0.connect_eth_peer_if(dpu_nic._eth_if)  # Port 2 = DPU
        compmap[HCOPSwitch] = HCOPSwitchSim
    else:
        # host_only: use a plain L2 switch to connect hosts
        plain_switch = system.EthSwitch(sys_b)
        plain_switch.connect_eth_peer_if(nic0._eth_if)
        plain_switch.connect_eth_peer_if(nic1._eth_if)
        compmap[system.EthSwitch] = sim_net.SwitchNet

    if has_dpu:
        compmap[DpuBMNIC] = DpuBMSim

    # ---- Build simulation ----
    simulation = sim_helpers.simple_simulation(
        sys_b,
        compmap=compmap,
        sync=True
    )

    sim_helpers.enable_sync_simulation(simulation)

    instantiation = inst_helpers.simple_instantiation(simulation)
    return instantiation

# ╔═══════════════════════════════════════════════════════════════════╗
# ║  Module entry point — read env vars, generate instantiation      ║
# ╚═══════════════════════════════════════════════════════════════════╝
placement = os.environ.get("HCOP_PLACEMENT", "switch_only")
primitive = os.environ.get("HCOP_PRIMITIVE", "paxos")
num_ops = int(os.environ.get("HCOP_NUM_OPS", "100"))

instantiations = [create_topology(placement, primitive, num_ops)]
