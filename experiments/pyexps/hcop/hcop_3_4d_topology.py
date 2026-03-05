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
            "placement_mode": "process_and_forward",
            "num_replicas": 3,
            "switch_node_id": 2,
            "barrier_default_participants": 2,
            "topology": {
               "fallback_port_index": 2,
               "nodes": [
                   {"id": 0, "port": 0},
                   {"id": 1, "port": 1}
               ]
            }
        }
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        import os
        os.makedirs(os.path.dirname(config_path), exist_ok=True)
        with open(config_path, 'w') as f:
            json.dump(config, f)

    def run_cmd(self, inst: inst_base.Instantiation) -> str:
        base_cmd = super().run_cmd(inst)
        config_path = f"{inst.env.get_simulator_output_dir(self)}/switch_config.json"
        return f"{base_cmd} -c {config_path}"

sys_d = system.System()
sys_d.name = "hcop_3_4d"
distro_disk_image = system.DistroDiskImage(sys_d, "hcop")

host0 = system.I40ELinuxHost(sys_d)
host0.name = "host0"
host0.add_disk(distro_disk_image)
host0.add_disk(system.LinuxConfigDiskImage(sys_d, host0))
nic0 = system.IntelI40eNIC(sys_d)
nic0.add_ipv4("10.0.0.1")
host0.connect_pcie_dev(nic0)

host1 = system.I40ELinuxHost(sys_d)
host1.name = "host1"
host1.add_disk(distro_disk_image)
host1.add_disk(system.LinuxConfigDiskImage(sys_d, host1))
nic1 = system.IntelI40eNIC(sys_d)
nic1.add_ipv4("10.0.0.2")
host1.connect_pcie_dev(nic1)

host2 = system.I40ELinuxHost(sys_d)
host2.name = "host-dpu"
host2.add_disk(distro_disk_image)
host2.add_disk(system.LinuxConfigDiskImage(sys_d, host2))
dpu_nic = DpuBMNIC(sys_d)
dpu_nic.add_ipv4("10.0.0.3")
host2.connect_pcie_dev(dpu_nic)

# Switch
switch0 = HCOPSwitch(sys_d)
switch0.connect_eth_peer_if(nic0._eth_if) # Port 0 (Node 0)
switch0.connect_eth_peer_if(nic1._eth_if) # Port 1 (Node 1)
switch0.connect_eth_peer_if(dpu_nic._eth_if) # Port 2 (DPU/Fallback)

# Barrier test: Host0 sends a BARRIER ARRIVE frame for barrier_id=1, N=2.
# Host1 sends a second BARRIER ARRIVE frame for barrier_id=1, N=2.
# The switch should count arrivals and broadcast RELEASE when count==N.

# Both hosts send a raw HCOP BARRIER frame, then listen for the RELEASE.

barrier_script_host0 = """
import socket, struct, time, sys

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(('eth0', 0))

dst_mac = b'\\xff\\xff\\xff\\xff\\xff\\xff'
src_mac = b'\\x04\\x23\\x7b\\x58\\x5a\\xa9'
ethertype = b'\\x88\\xb5'

# BarrierMsgHeader (8 bytes per barrier_proto.h):
#   msg_type(u8)=1(ARRIVE), sender_id(u8)=0, generation(u16)=0, barrier_id(u32)=1
barrier_payload = struct.pack('<BBHI', 1, 0, 0, 1)

# HcopHeader (16 bytes): primitive_type=3(BARRIER), exception_type=0, operation_id=100, source_tier=0, num_tier_crossings=0, tier_path=0, payload_len
hcop_hdr = struct.pack('<HHIBBIH', 3, 0, 100, 0, 0, 0, len(barrier_payload))
frame = dst_mac + src_mac + ethertype + hcop_hdr + barrier_payload

time.sleep(3)
print("HOST0: Sending BARRIER ARRIVE sender_id=0 for barrier_id=1")
sys.stdout.flush()
s.send(frame)

# Wait for RELEASE (msg_type=2) from the switch
s.settimeout(30.0)
try:
    while True:
        pkt = s.recv(2048)
        if len(pkt) > 14 and pkt[12:14] == b'\\x88\\xb5':
            ctype, exc = struct.unpack('<HH', pkt[14:18])
            if ctype == 3:
                # BarrierMsgHeader starts at offset 14+16=30
                bmsg_type = pkt[30]
                print(f"HOST0: Received barrier msg_type={bmsg_type}")
                sys.stdout.flush()
                if bmsg_type == 2:
                    print("SUCCESS_BARRIER_RELEASE_RECEIVED_HOST0")
                    sys.stdout.flush()
                    break
except Exception as e:
    print("HOST0: Error or timeout:", e)
    sys.stdout.flush()
"""

barrier_script_host1 = """
import socket, struct, time, sys

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(('eth0', 0))

dst_mac = b'\\xff\\xff\\xff\\xff\\xff\\xff'
src_mac = b'\\x1c\\xcf\\x9c\\xfa\\x4f\\xf6'
ethertype = b'\\x88\\xb5'

# BarrierMsgHeader: msg_type=1(ARRIVE), sender_id=1, generation=0, barrier_id=1
barrier_payload = struct.pack('<BBHI', 1, 1, 0, 1)

# HcopHeader: primitive_type=3(BARRIER), exception_type=0, operation_id=101, source_tier=0, num_tier_crossings=0, tier_path=0, payload_len
hcop_hdr = struct.pack('<HHIBBIH', 3, 0, 101, 0, 0, 0, len(barrier_payload))
frame = dst_mac + src_mac + ethertype + hcop_hdr + barrier_payload

time.sleep(3)
print("HOST1: Sending BARRIER ARRIVE sender_id=1 for barrier_id=1")
sys.stdout.flush()
s.send(frame)

# Wait for RELEASE
s.settimeout(30.0)
try:
    while True:
        pkt = s.recv(2048)
        if len(pkt) > 14 and pkt[12:14] == b'\\x88\\xb5':
            ctype, exc = struct.unpack('<HH', pkt[14:18])
            if ctype == 3:
                bmsg_type = pkt[30]
                print(f"HOST1: Received barrier msg_type={bmsg_type}")
                sys.stdout.flush()
                if bmsg_type == 2:
                    print("SUCCESS_BARRIER_RELEASE_RECEIVED_HOST1")
                    sys.stdout.flush()
                    break
except Exception as e:
    print("HOST1: Error or timeout:", e)
    sys.stdout.flush()
"""

from simbricks.orchestration.system.host import app

class BarrierHost0App(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            "cat << 'EOF' > /root/barrier_test.py",
            barrier_script_host0,
            "EOF",
            "python3 -u /root/barrier_test.py"
        ]

class BarrierHost1App(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            "cat << 'EOF' > /root/barrier_test.py",
            barrier_script_host1,
            "EOF",
            "python3 -u /root/barrier_test.py"
        ]

app0 = BarrierHost0App(host0)
app0.wait = True
host0.add_app(app0)

app1 = BarrierHost1App(host1)
app1.wait = True
host1.add_app(app1)

host2.add_app(system.Sleep(host2, infinite=True))

simulation = sim_helpers.simple_simulation(
    sys_d,
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
