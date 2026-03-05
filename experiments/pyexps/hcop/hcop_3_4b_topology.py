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
            "telemetry_interval_ms": 10,
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

sys_b = system.System()
sys_b.name = "hcop_3_4b"
distro_disk_image = system.DistroDiskImage(sys_b, "hcop")

host0 = system.I40ELinuxHost(sys_b)
host0.name = "host0"
host0.add_disk(distro_disk_image)
host0.add_disk(system.LinuxConfigDiskImage(sys_b, host0))
nic0 = system.IntelI40eNIC(sys_b)
nic0.add_ipv4("10.0.0.1")
host0.connect_pcie_dev(nic0)

host1 = system.I40ELinuxHost(sys_b)
host1.name = "host1"
host1.add_disk(distro_disk_image)
host1.add_disk(system.LinuxConfigDiskImage(sys_b, host1))
nic1 = system.IntelI40eNIC(sys_b)
nic1.add_ipv4("10.0.0.2")
host1.connect_pcie_dev(nic1)

host2 = system.I40ELinuxHost(sys_b)
host2.name = "host-dpu"
host2.add_disk(distro_disk_image)
host2.add_disk(system.LinuxConfigDiskImage(sys_b, host2))
dpu_nic = DpuBMNIC(sys_b)
dpu_nic.add_ipv4("10.0.0.3")
host2.connect_pcie_dev(dpu_nic)

# Switch
switch0 = HCOPSwitch(sys_b)
switch0.connect_eth_peer_if(nic0._eth_if) # Port 0 (Node 0)
switch0.connect_eth_peer_if(nic1._eth_if) # Port 1 (Node 1)
switch0.connect_eth_peer_if(dpu_nic._eth_if) # Port 2 (DPU/Fallback)

# Host0 runs a python script sending the raw ethernet frame
py_script = """
import socket, struct, time

# 0x0003 is ETH_P_ALL. Python needs it in network byte order: socket.ntohs(0x0003)
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(('eth0', 0))

dst_mac = b'\\xff\\xff\\xff\\xff\\xff\\xff'
src_mac = b'\\x04\\x23\\x7b\\x58\\x5a\\xa9'
ethertype = b'\\x88\\xb5'

# HcopHeader (8 bytes): type=1, ex=0, op=42, tier=0, flags=0, plen=28
hcop_hdr = struct.pack('<BBHBBH', 1, 0, 42, 0, 0, 24)

# PaxosMsgHeader + AcceptMsg (value is 4 bytes)
# msg_type=3 (ACCEPT), sender_id=0, num_replicas=2, instance_id=0
# proposal (round=1, node=0, pad=0)=6 bytes+2 bytes padding
# value_len=4, value=b'TEST' padding up to 128? Not necessary if we just omit trailing zeroes? Wait value_len determines logical length, struct is full length?
# Actually paxos_proto.h says value[kMaxValueSize] which is 128 bytes. Length of AcceptMsg is 8+8+2+128 = 146 bytes.
# Let's pack exactly 146 bytes.
paxos_hdr = struct.pack('<BBHI', 3, 0, 2, 0)
proposal = struct.pack('<IHH', 1, 0, 0)
val_len = struct.pack('<H', 4)
val = b'TEST' + b'\\x00' * 124

payload = paxos_hdr + proposal + val_len + val

# HcopHeader (16 bytes): type=1, ex=0, op=42, tier=0, num_tier_crossings=0, tier_path=0, plen=...
hcop_hdr = struct.pack('<HHIBBIH', 1, 0, 42, 0, 0, 0, len(payload))
frame = dst_mac + src_mac + ethertype + hcop_hdr + payload

time.sleep(2)  # Wait for link up
import time as time_mod
print("Sending HCOP ACCEPT frame")
start_time_ns = time_mod.time_ns()
s.send(frame)

# Wait for 1 second and sniff for ACCEPTED (msg_type=4)
# In non-blocking or just simple loop
s.settimeout(30.0)
try:
    while True:
        pkt = s.recv(2048)
        print(f"Received packet of length {len(pkt)}")
        if len(pkt) > 14 and pkt[12:14] == b'\\x88\\xb5':
            print(f"  EtherType is HCOP. HCOP type: {pkt[14]}, HCOP ex: {pkt[15]}")
            # HcopHeader is 16 bytes. payload starts at 14+16 = 30
            ctype, exc = struct.unpack('<HH', pkt[14:18])
            if ctype == 1: # Paxos
                print(f"  HCOP type is Paxos. Paxos msg_type: {pkt[30]}")
                # Paxos message type is first byte of payload
                msg_type = pkt[30]
                if msg_type == 4: # ACCEPTED
                    end_time_ns = time_mod.time_ns()
                    import sys, os
                    
                    # Unpack HCOP header to get tier path and crossings
                    _, exc_type, op_id, _, crossings, tier_path, _ = struct.unpack('<HHIBBIH', pkt[14:30])
                    
                    # Decode tier path
                    path_str = ""
                    if crossings == 0:
                        path_str = "Unknown"
                    else:
                        path_list = []
                        for i in range(crossings - 1, -1, -1):
                            nibble = (tier_path >> (i * 4)) & 0x0F
                            if nibble == 0: path_list.append("S")
                            elif nibble == 1: path_list.append("D")
                            elif nibble == 2: path_list.append("H")
                            else: path_list.append("?")
                        path_str = "->".join(path_list)
                    
                    latency = end_time_ns - start_time_ns
                    was_exc = 1 if exc_type != 0 else 0
                    
                    # Write to CSV
                    csv_path = "/tmp/hcop_operations.csv"
                    write_header = not os.path.exists(csv_path)
                    with open(csv_path, "a") as f:
                        if write_header:
                            f.write("operation_id,primitive_type,placement_config,start_time_ns,end_time_ns,latency_ns,tier_path,num_tier_crossings,was_exception,exception_type\\n")
                        f.write(f"{op_id},1,unknown,{start_time_ns},{end_time_ns},{latency},{path_str},{crossings},{was_exc},{exc_type}\\n")
                    
                    print("SUCCESS_RECEIVED_ACCEPTED_FROM_SWITCH")
                    sys.stdout.flush()
                    # Sleep to let host1 timeout and print CSV
                    time_mod.sleep(4)
                    break
except Exception as e:
    import traceback
    traceback.print_exc()
    print("Error or timeout:", e)
"""

from simbricks.orchestration.system.host import app

class RawHcopClientApp(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            "ls -la /usr/local/bin/paxos_host",
            "paxos_host --client -i 0 -r 2 -t 10.0.0.1 -v 42 -f eth0 -m raw -o -n 20",
            "cat /tmp/hcop_operations.csv",
            "cat /tmp/hcop_utilization_host.csv"
        ]

app0 = RawHcopClientApp(host0)
app0.wait = True
host0.add_app(app0)

class PaxosHostServerApp(app.GenericRawCommandApplication):
    def run_cmds(self, inst) -> list[str]:
        return [
            "timeout 5s paxos_host --server --interval 10 --iface eth0 -m raw",
            "cat /tmp/hcop_utilization_host.csv"
        ]

app1 = PaxosHostServerApp(host1)
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
    sync=True
)

# Enable time synchronization so simulated time advances correctly
sys_b.sync = True
simulation.sync = True

instantiation = inst_helpers.simple_instantiation(simulation)
fragment = inst.Fragment()
fragment.add_simulators(*simulation.all_simulators())
instantiation.fragments = [fragment]
instantiations = [instantiation]
