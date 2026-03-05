import json
import sys

def dump_node(path, node_name):
    try:
        with open(path) as f:
            d = json.load(f)
    except Exception as e:
        return
    
    if node_name in d:
        print(f"--- {node_name} ---")
        out_list = d[node_name]
        if isinstance(out_list, list):
            for log in out_list:
                if 'stdout' in log:
                    print("\n".join(log['stdout']))
        elif isinstance(out_list, dict) and 'stdout' in out_list:
              print("\n".join(out_list['stdout']))
        elif isinstance(out_list, dict) and 'output' in out_list:
            for log in out_list['output']:
                if 'stdout' in log:
                    print("\n".join(log['stdout']))
                    
dump_node(sys.argv[1], "host.host0")
dump_node(sys.argv[1], "host.host1")
dump_node(sys.argv[1], "host.host-dpu")
dump_node(sys.argv[1], "nic.DpuBMSim-39")
dump_node(sys.argv[1], "nic.NICSim-35")
dump_node(sys.argv[1], "nic.NICSim-37")
