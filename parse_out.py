import sys
import json

data = json.load(open(sys.argv[1]))
for k, v in data.items():
    if not isinstance(v, dict): continue
    print(f"\n=== {k} ===")
    out_blocks = v.get("output", [])
    for block in out_blocks:
        print("".join(block.get("stdout", [])))
