#!/usr/bin/env python3
"""Clean up summary CSVs: remove N/A/MISSING/unknown/duplicate rows, keep only the best valid row."""
import csv
import os
import glob

RESULTS = "/home/user/Documents/simbricks/results"

for csv_path in sorted(glob.glob(os.path.join(RESULTS, "*", "*", "summary.csv"))):
    with open(csv_path) as f:
        reader = csv.reader(f)
        rows = list(reader)
    
    if len(rows) < 2:
        continue
    
    header = rows[0]
    # Find valid data rows (not N/A in latency_p50_ns column, not MISSING workload)
    p50_idx = header.index("latency_p50_ns")
    wp_idx = header.index("workload_params")
    
    valid = []
    for row in rows[1:]:
        if len(row) < len(header):
            continue
        if row[p50_idx] == "N/A" or row[p50_idx] == "":
            continue
        if row[wp_idx] == "MISSING" or row[wp_idx] == "unknown":
            continue
        valid.append(row)
    
    if not valid:
        print(f"WARNING: {csv_path} has NO valid data rows, skipping")
        continue
    
    # Keep only the last valid row (newest data)
    best = valid[-1]
    
    # Write clean file
    with open(csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerow(best)
    
    rel = os.path.relpath(csv_path, RESULTS)
    print(f"  Cleaned {rel}: {best[0]}/{best[1]} {best[wp_idx]} p50={best[p50_idx]}")

# Now generate master_summary.csv
print("\n=== Generating master_summary.csv ===")
master_path = os.path.join(RESULTS, "master_summary.csv")
header_written = False
row_count = 0

with open(master_path, 'w', newline='') as mf:
    writer = csv.writer(mf)
    for csv_path in sorted(glob.glob(os.path.join(RESULTS, "*", "*", "summary.csv"))):
        with open(csv_path) as f:
            reader = csv.reader(f)
            rows = list(reader)
        if len(rows) < 2:
            continue
        if not header_written:
            writer.writerow(rows[0])
            header_written = True
        for row in rows[1:]:
            writer.writerow(row)
            row_count += 1

print(f"  Written {master_path} with {row_count} data rows")

# Verify
print("\n=== Verification ===")
with open(master_path) as f:
    reader = csv.reader(f)
    header = next(reader)
    rows = list(reader)

print(f"  Total rows: {len(rows)}")

p50_idx = header.index("latency_p50_ns")
wp_idx = header.index("workload_params")
pc_idx = header.index("placement_config")
pt_idx = header.index("primitive_type")

na_rows = [r for r in rows if r[p50_idx] == "N/A"]
unk_rows = [r for r in rows if r[wp_idx] == "unknown" or r[pc_idx] == "unknown"]
bad_prim = [r for r in rows if r[pt_idx] not in ("paxos", "lock", "barrier")]
not_n100 = [r for r in rows if r[wp_idx] != "n=100"]

print(f"  N/A p50 rows: {len(na_rows)}")
print(f"  Unknown rows: {len(unk_rows)}")
print(f"  Bad primitive: {len(bad_prim)}")
print(f"  Not n=100: {len(not_n100)}")

if len(rows) == 21 and not na_rows and not unk_rows and not bad_prim and not not_n100:
    print("  ✅ ALL CHECKS PASS")
else:
    print("  ❌ SOME CHECKS FAILED")
    for r in rows:
        if r in na_rows or r in unk_rows or r in bad_prim or r in not_n100:
            print(f"    BAD: {r[pc_idx]}/{r[pt_idx]} wp={r[wp_idx]} p50={r[p50_idx]}")
