import time
import sys

log_file = "experiments/out/master_rerun.log"
timeout = 600
start_time = time.time()

while time.time() - start_time < timeout:
    try:
        with open(log_file, "r") as f:
            content = f.read()
            if "Simulation completed" in content or "ERROR" in content or "host_only" in content[-200:]:
                print(content[-500:])
                sys.exit(0)
    except FileNotFoundError:
        pass
    time.sleep(10)

print("Timeout reached.")
sys.exit(1)
