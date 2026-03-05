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

sys = system.System()
sys.name = "simple_ping"
distro_disk_image = system.DistroDiskImage(sys, "base")

host0 = system.I40ELinuxHost(sys)
host0.name = "host0"
host0.add_disk(distro_disk_image)
host0.add_disk(system.LinuxConfigDiskImage(sys, host0))
nic0 = system.IntelI40eNIC(sys)
nic0.add_ipv4("10.0.0.1")
host0.connect_pcie_dev(nic0)

host1 = system.I40ELinuxHost(sys)
host1.name = "host1"
host1.add_disk(distro_disk_image)
host1.add_disk(system.LinuxConfigDiskImage(sys, host1))
nic1 = system.IntelI40eNIC(sys)
nic1.add_ipv4("10.0.0.2")
host1.connect_pcie_dev(nic1)

switch0 = system.EthSwitch(sys)
switch0.connect_eth_peer_if(nic0._eth_if)
switch0.connect_eth_peer_if(nic1._eth_if)

sleep_app = system.Sleep(host0, infinite=True)
sleep_app.wait = False
host0.add_app(sleep_app)

ping_app = system.PingClient(host1, "10.0.0.1")
ping_app.wait = True
host1.add_app(ping_app)

simulation = sim_helpers.simple_simulation(
    sys,
    compmap={
        system.FullSystemHost: sim.QemuSim,
        system.IntelI40eNIC: sim.I40eNicSim,
        system.EthSwitch: sim_net.SwitchNet,
    },
)

instantiation = inst_helpers.simple_instantiation(simulation)
fragment = inst.Fragment()
fragment.add_simulators(*simulation.all_simulators())
instantiation.fragments = [fragment]

instantiations = [instantiation]
