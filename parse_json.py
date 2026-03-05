import json, sys, os

def run():
    out_dir = sys.argv[1]
    path = os.path.join(out_dir, 'output', 'out.json')
    print("Parsing:", path)
    with open(path) as f:
        d = json.load(f)

    ops = []
    utils = []
    
    # Check both host0 and host1
    for k in ['host.host0', 'host.host1']:
        for o in d.get(k, {}).get('output', []):
            in_ops = False
            in_util = False
            for line in o.get('stdout', []):
                line = line.strip()
                if line == "--- BEGIN hcop_operations.csv ---":
                    in_ops = True
                    continue
                if line == "--- END hcop_operations.csv ---":
                    in_ops = False
                    continue
                if line == "--- BEGIN hcop_utilization_host.csv ---":
                    in_util = True
                    continue
                if line == "--- END hcop_utilization_host.csv ---":
                    in_util = False
                    continue
                    
                if in_ops and len(line) > 0: ops.append(line)
                if in_util and len(line) > 0: utils.append(line)

    header_ops = ""
    header_utils = ""
    
    clean_ops = []
    for x in ops:
        if x.startswith("operation_id"):
            header_ops = x
        else:
            clean_ops.append(x)
            
    clean_utils = []
    for x in utils:
        if x.startswith("timestamp_ns"):
            header_utils = x
        else:
            clean_utils.append(x)
            
    clean_ops = list(set(clean_ops))
    clean_utils = list(set(clean_utils))
    
    if header_ops:
        clean_ops.insert(0, header_ops)
    if header_utils:
        clean_utils.insert(0, header_utils)
    
    with open('/tmp/hcop_operations.csv', 'w') as f:
        f.write('\n'.join(clean_ops) + '\n')
    
    with open('/tmp/hcop_utilization_host.csv', 'w') as f:
        f.write('\n'.join(clean_utils) + '\n')
        
    print(f"Extraction completed. {len(clean_ops)} ops, {len(clean_utils)} utils.")

if __name__ == '__main__':
    run()
