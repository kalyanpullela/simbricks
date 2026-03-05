import subprocess
import os
subprocess.run([
    "/home/user/.local/bin/simbricks-run", 
    "--repo", "/home/user/Documents/simbricks/", 
    "--verbose", 
    "--force", 
    "pyexps/hcop/hcop_3_4b_topology.py"
], cwd="/home/user/Documents/simbricks/experiments", env={"PYTHONPATH": "/home/user/Documents/simbricks/symphony/"})
