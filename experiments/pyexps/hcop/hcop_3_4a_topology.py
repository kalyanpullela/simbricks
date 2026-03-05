import sys
import os
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

# Wrapper for DPU BM
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

    def add(self, nic: DpuBMNIC):
        super().add(nic)

# Wrapper for HCOP Switch
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
        import json
        await super().prepare(inst)
        config = {
            "sram_pages_total": 1024,
            "tcam_blocks_total": 64,
            "placement_mode": "forward_only",
            "fallback_port_index": 2
        }
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        import os
        os.makedirs(os.path.dirname(config_path), exist_ok=True)
        with open(config_path, 'w') as f:
            json.dump(config, f)

    def run_cmd(self, inst: inst_base.Instantiation) -> str:
        base_cmd = super().run_cmd(inst)
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        # base_cmd is usually `executable ...`. We can just append to the base_cmd or insert.
        # SwitchNet run_cmd does: f"{inst.env.repo_base(self._executable)} -S {sync_period} ..."
        return f"{base_cmd} -c {config_path}"

sys = system.System()
sys.name = "hcop"
distro_disk_image = system.DistroDiskImage(sys, "base")

# 1. Host 0 (Client 0)
host0 = system.I40ELinuxHost(sys)
host0.name = "host0"
host0.add_disk(distro_disk_image)
host0.add_disk(system.LinuxConfigDiskImage(sys, host0))
nic0 = system.IntelI40eNIC(sys)
nic0.add_ipv4("10.0.0.1")
host0.connect_pcie_dev(nic0)

# 2. Host 1 (Client 1)
host1 = system.I40ELinuxHost(sys)
host1.name = "host1"
host1.add_disk(distro_disk_image)
host1.add_disk(system.LinuxConfigDiskImage(sys, host1))
nic1 = system.IntelI40eNIC(sys)
nic1.add_ipv4("10.0.0.2")
host1.connect_pcie_dev(nic1)

# 3. Host 2 (DPU Host)
host2 = system.I40ELinuxHost(sys)
host2.name = "host-dpu"
host2.add_disk(distro_disk_image)
host2.add_disk(system.LinuxConfigDiskImage(sys, host2))
dpu_nic = DpuBMNIC(sys)
dpu_nic.add_ipv4("10.0.0.3")
host2.connect_pcie_dev(dpu_nic)

# Switch
switch0 = HCOPSwitch(sys)

switch0.connect_eth_peer_if(nic0._eth_if) # Port 0
switch0.connect_eth_peer_if(nic1._eth_if) # Port 1

# If we need DPU on exactly port 3, we add a dummy device to port 2.
switch0.connect_eth_peer_if(dpu_nic._eth_if) # Port 2

app0 = system.PingClient(host0, nic1._ip)
app0.wait = True
host0.add_app(app0)
host1.add_app(system.Sleep(host1, infinite=True))
host2.add_app(system.Sleep(host2, infinite=True))

simulation = sim_helpers.simple_simulation(
    sys,
    compmap={
        system.FullSystemHost: sim.QemuSim,
        system.IntelI40eNIC: sim.I40eNicSim,
        DpuBMNIC: DpuBMSim,
        HCOPSwitch: HCOPSwitchSim,
    },
)

instantiation = inst_helpers.simple_instantiation(simulation)
fragment = inst.Fragment()
fragment.add_simulators(*simulation.all_simulators())
instantiation.fragments = [fragment]

instantiations = [instantiation]
for sim_inst in simulation.all_simulators():
    print(f"DEBUG: Simulator {type(sim_inst)} handles {[type(c) for c in sim_inst.components()]}")
