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
        import os
        await super().prepare(inst)
        config = {
            "sram_pages_total": 1024,
            "tcam_blocks_total": 64,
            "placement_mode": "process_and_forward",
            "num_replicas": 3,
            "switch_node_id": 2,
            "lock_entry_sram_pages": 4, # ensure lock state works
            "topology": {
               "fallback_port_index": 2,
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

sys_b = system.System()
sys_b.name = "hcop_3_4c"
distro_disk_image = system.DistroDiskImage(sys_b, "hcop")

# Host 0
host0 = system.I40ELinuxHost(sys_b)
host0.name = "host0"
host0.add_disk(distro_disk_image)
host0.add_disk(system.LinuxConfigDiskImage(sys_b, host0))
nic0 = system.IntelI40eNIC(sys_b)
nic0.add_ipv4("10.0.0.1")
host0.connect_pcie_dev(nic0)

# Host 1
host1 = system.I40ELinuxHost(sys_b)
host1.name = "host1"
host1.add_disk(distro_disk_image)
host1.add_disk(system.LinuxConfigDiskImage(sys_b, host1))
nic1 = system.IntelI40eNIC(sys_b)
nic1.add_ipv4("10.0.0.2")
host1.connect_pcie_dev(nic1)

# DPU
host2 = system.I40ELinuxHost(sys_b)
host2.name = "host-dpu"
host2.add_disk(distro_disk_image)
host2.add_disk(system.LinuxConfigDiskImage(sys_b, host2))
dpu_nic = DpuBMNIC(sys_b)
dpu_nic.add_ipv4("10.0.0.3")
host2.connect_pcie_dev(dpu_nic)

switch0 = HCOPSwitch(sys_b)
switch0.connect_eth_peer_if(nic0._eth_if) # Port 0 -> Host 0
switch0.connect_eth_peer_if(nic1._eth_if) # Port 1 -> Host 1
switch0.connect_eth_peer_if(dpu_nic._eth_if) # Port 2 -> DPU

class Host0App(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            # Wait for network link
            "sleep 3",
            "echo '--- HOST 0 ACQUIRING LOCK 42 ---'",
            "lock_host --client --id=0 --lock=42 --op=acquire --transport=raw",
            # Hold lock for 1 second, then release
            "sleep 1",
            "echo '--- HOST 0 RELEASING LOCK 42 ---'",
            "lock_host --client --id=0 --lock=42 --op=release --transport=raw"
        ]

class Host1App(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            "sleep 3",
            "cat << 'EOF' > sniff.py\n"
            "import socket, struct, sys\n"
            "s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))\n"
            "s.bind(('eth0', 0))\n"
            "s.settimeout(15.0)\n"
            "print('SNIFFER_STARTED')\n"
            "sys.stdout.flush()\n"
            "try:\n"
            "  while True:\n"
            "    pkt = s.recv(2048)\n"
            "    if len(pkt) > 14 and pkt[12:14] == b'\\x88\\xb5':\n"
            "      ctype, exc = struct.unpack('<HH', pkt[14:18])\n"
            "      if ctype == 2: # Lock\n"
            "        msg_type = pkt[30]\n"
            "        if msg_type == 2: # GRANT\n"
            "          print('SUCCESS_GRANT_RECEIVED')\n"
            "          sys.stdout.flush()\n"
            "          break\n"
            "except Exception as e:\n"
            "  pass\n"
            "EOF\n",
            "python3 -u sniff.py &",
            "sleep 0.5",
            "echo '--- HOST 1 ATTEMPTING TO ACQUIRE LOCK 42 (CONTENTION) ---'",
            "timeout 2 lock_host --client --id=1 --lock=42 --op=acquire --transport=raw || true",
            "echo '--- HOST 1 WAIT FOR AUTO-GRANT ---'",
            "wait"
        ]

app0 = Host0App(host0)
app0.wait = True
host0.add_app(app0)

app1 = Host1App(host1)
app1.wait = True
host1.add_app(app1)

host2.add_app(system.Sleep(host2, infinite=True))

simulation = sim_helpers.simple_simulation(
    sys_b,
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
