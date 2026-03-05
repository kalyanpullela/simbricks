import os
import csv
import glob

def generate_master():
    results_dir = "results"
    primitives = ["paxos", "lock", "barrier"]
    placements = ["switch_only", "switch_host", "dpu_only", "dpu_host", "host_only", "switch_dpu", "switch_dpu_host"]
    
    header = [
        "placement_config", "primitive_type", "workload_params", "latency_mean_ns", "latency_p50_ns", "latency_p99_ns",
        "throughput_ops_per_sec", "host_cpu_utilization_pct", "host_cpu_utilization_peak_pct",
        "switch_stage_utilization_pct", "switch_stage_utilization_peak_pct",
        "dpu_core_utilization_pct", "dpu_core_utilization_peak_pct",
        "dpu_memory_utilization_pct", "dpu_memory_utilization_peak_pct", "exception_rate_pct", "overflow_rate_pct"
    ]
    
    rows = []
    
    # Define preferred order for placements (per USER request/intuition)
    placement_order = ["switch_only", "switch_host", "switch_dpu", "switch_dpu_host", "dpu_only", "dpu_host", "host_only"]
    
    # Priority: Primitive, then Placement
    for prim in primitives:
        for plac in placement_order:
            summary_path = os.path.join(results_dir, prim, plac, "summary.csv")
            if os.path.exists(summary_path):
                with open(summary_path, 'r') as f:
                    reader = csv.reader(f)
                    next(reader) # skip header
                    for row in reader:
                        if len(row) > 0:
                            rows.append(row)
            else:
                missing_row = [plac, prim, "MISSING"] + ["N/A"] * (len(header) - 3)
                rows.append(missing_row)
    
    # Final master CSV
    master_path = os.path.join(results_dir, "master_summary.csv")
    with open(master_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)
    
    # Print a markdown table for the user
    print("| Placement | Primitive | Mean Latency (ns) | P50 (ns) | P99 (ns) | Throughput (ops/s) |")
    print("|-----------|-----------|-------------------|----------|----------|-------------------|")
    for r in rows:
        if r[2] == "MISSING":
            print(f"| {r[0]} | {r[1]} | **MISSING** | - | - | - |")
        else:
            # latency_mean_ns, latency_p50_ns, latency_p99_ns, throughput_ops_per_sec
            try:
                mean = f"{float(r[3]):.1f}"
                p50 = f"{float(r[4]):.1f}"
                p99 = f"{float(r[5]):.1f}"
                tp = f"{float(r[6]):.1f}"
            except:
                mean, p50, p99, tp = "Err", "Err", "Err", "Err"
            print(f"| {r[0]} | {r[1]} | {mean} | {p50} | {p99} | {tp} |")

if __name__ == "__main__":
    generate_master()
