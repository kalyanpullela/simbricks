import argparse
import sys
import simbricks.orchestration.experiments as exp
import simbricks.orchestration.nodeconfig as node
import simbricks.orchestration.simulators as sim

class HcopPingNode(node.NodeConfig):
    def __init__(self, run_cmd):
        super().__init__()
        self.run_cmd = run_cmd
        self.memory = 2048

    def prepare_pre_cp(self):
        return [
            "mount -t debugfs none /sys/kernel/debug",
            "ip link set dev eth0 up",
            f"ip addr add {self.ip}/24 dev eth0",
            # We add a sleep just to allow interfaces to come up.
            "sleep 2"
        ]

    def run(self):
        # We append a 'sleep' so that the host does not shut down before
        # the packets can traverse the network. Wait for terminal shutdown signal.
        return self.run_cmd + "; sleep infinity"


paxos_e = exp.Experiment("hcop_3_4d")

# 1. Switch
switch = sim.HCOPSwitchSim("switch")
switch.sync = False
switch.multicast = True
paxos_e.add_pcidev(switch)

# 2. DPU (acting as host/fpu)
dpu_config = HcopPingNode("barrier_host --client --test-basic --barrier 1 --participants 2")
dpu_config.ip = "10.0.0.3"
dpu_host = sim.QemuSim("host-dpu")
dpu_host.name = "host-dpu"
dpu_host.sync = False
paxos_e.add_host(dpu_host)
dpu_host.set_config(dpu_config)

dpu_nic = sim.DpuBMSim("dpu-nic")
dpu_nic.sync = False
dpu_nic.set_network(switch)
dpu_host.add_nic(dpu_nic)
paxos_e.add_nic(dpu_nic)

# 3. Host 0
host0_config = HcopPingNode("barrier_host --client --test-basic --barrier 1 --participants 2")
host0_config.ip = "10.0.0.1"
host0 = sim.QemuSim("host0")
host0.name = "host0"
host0.sync = False
paxos_e.add_host(host0)
host0.set_config(host0_config)

host0_nic = sim.I40eNicSim("nic0")
host0_nic.sync = False
host0_nic.set_network(switch)
host0.add_nic(host0_nic)
paxos_e.add_nic(host0_nic)

# 4. Host 1
host1_config = HcopPingNode("barrier_host --client --test-basic --barrier 1 --participants 2")
host1_config.ip = "10.0.0.2"
host1 = sim.QemuSim("host1")
host1.name = "host1"
host1.sync = False
paxos_e.add_host(host1)
host1.set_config(host1_config)

host1_nic = sim.I40eNicSim("nic1")
host1_nic.sync = False
host1_nic.set_network(switch)
host1.add_nic(host1_nic)
paxos_e.add_nic(host1_nic)

print(paxos_e.name)
paxos_e.timeout = 120
print(f"Added experiment: {paxos_e.name}")

experiments = [paxos_e]
