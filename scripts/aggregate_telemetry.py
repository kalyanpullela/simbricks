#!/usr/bin/env python3
import argparse
import csv
import glob
import math
import os
import statistics
import sys

# Exception mapped values from hcop_proto.h used to distinguish overflow from exception
OVERFLOW_EXCEPTIONS = {
    1: {2}, # kPrimitivePaxos: kPaxosStateOverflow (2)
    2: {2}, # kPrimitiveLock: kLockStateOverflow (2)
    3: {1}  # kPrimitiveBarrier: kBarrierGenerationOverflow (1)
}
K_DPU_CORE_EXHAUSTED = 0x0100

def parse_args():
    parser = argparse.ArgumentParser(description="Aggregate simulation telemetry")
    parser.add_argument("--run-dir", required=True, help="Directory containing run output CSVs")
    parser.add_argument("--warmup-seconds", type=float, default=0.0, help="Simulation time to ignore at the start")
    parser.add_argument("--out", required=True, help="Path to write the summary CSV")
    parser.add_argument("--placement", default="unknown", help="Override placement_config name")
    parser.add_argument("--primitive", default="unknown", help="Override primitive_type name")
    parser.add_argument("--workload", default="unknown", help="Override workload_params")
    return parser.parse_args()

def analyze_operations(ops_path, warmup_seconds):
    metrics = {
        'throughput_ops_per_sec': 'N/A',
        'latency_mean_ns': 'N/A',
        'latency_p50_ns': 'N/A',
        'latency_p99_ns': 'N/A',
        'exception_rate_pct': 'N/A',
        'overflow_rate_pct': 'N/A',
        'placement_config': 'unknown',
        'primitive_type': 'unknown',
        'workload_params': 'unknown'
    }
    
    if not os.path.exists(ops_path):
        print(f"Warning: {ops_path} not found.")
        return metrics

    latencies = []
    total_ops = 0
    exceptions = 0
    overflows = 0
    
    first_start = None
    last_end = None
    
    with open(ops_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            start_time = float(row['start_time_ns'])
            if first_start is None:
                first_start = start_time
            
            # Warm-up filter
            if (start_time - first_start) < (warmup_seconds * 1e9):
                continue
                
            total_ops += 1
            lat = float(row['latency_ns'])
            latencies.append(lat)
            
            last_end = float(row['end_time_ns'])
            
            # Record config info if available
            metrics['placement_config'] = row.get('placement_config', 'unknown')
            metrics['primitive_type'] = row.get('primitive_type', 'unknown')
            # Extract primitive integer type if available, fallback to 0
            primitive_val = 0
            # Assuming 'primitive_type' is a string we might map to an int, or we parse from row if it has an ID
            # Let's see if there is an integer primitive type in operations CSV
            # Current schema: operation_id, primitive_type, ...
            try:
                primitive_val = int(row.get('primitive_type', '0'))
            except ValueError:
                prim_str = row.get('primitive_type', '').lower()
                if 'paxos' in prim_str: primitive_val = 1
                elif 'lock' in prim_str: primitive_val = 2
                elif 'barrier' in prim_str: primitive_val = 3
            
            was_exception = (row.get('was_exception', '0') == '1' or row.get('was_exception', 'False') == 'True')
            if was_exception:
                exceptions += 1
                ex_type = int(row.get('exception_type', '0'))
                
                # Check for overflow
                is_overflow = False
                if ex_type == K_DPU_CORE_EXHAUSTED:
                    is_overflow = True
                elif primitive_val in OVERFLOW_EXCEPTIONS and ex_type in OVERFLOW_EXCEPTIONS[primitive_val]:
                    is_overflow = True
                    
                if is_overflow:
                    overflows += 1

    if total_ops < 2:
        return metrics
        
    latencies.sort()
    metrics['latency_mean_ns'] = statistics.mean(latencies)
    
    def percentile(data, p):
        k = (len(data) - 1) * p
        f = math.floor(k)
        c = math.ceil(k)
        if f == c:
            return data[int(k)]
        d0 = data[int(f)] * (c - k)
        d1 = data[int(c)] * (k - f)
        return d0 + d1
        
    metrics['latency_p50_ns'] = percentile(latencies, 0.50)
    metrics['latency_p99_ns'] = percentile(latencies, 0.99)
    
    metrics['exception_rate_pct'] = (exceptions / total_ops) * 100.0
    metrics['overflow_rate_pct'] = (overflows / total_ops) * 100.0
    
    # Calculate throughput using start of post-warmup and end of post-warmup
    duration_s = (last_end - first_start - (warmup_seconds * 1e9)) / 1e9
    if duration_s > 0:
        metrics['throughput_ops_per_sec'] = total_ops / duration_s
        
    return metrics

def analyze_utilization(csv_path, warmup_seconds, prefix):
    metrics = {
        f'{prefix}_utilization_pct': 'NULL',
        f'{prefix}_utilization_peak_pct': 'NULL'
    }
    # Optional memory utilization for tiers that have it
    mem_prefix = prefix.replace('switch_stage', 'switch_memory').replace('host_cpu', 'host_memory').replace('dpu_core', 'dpu_memory')
    if 'memory' not in mem_prefix: # Fallback just in case
        mem_prefix = f"{prefix}_memory"
        
    metrics.update({
        f'{mem_prefix}_utilization_pct': 'NULL',
        f'{mem_prefix}_utilization_peak_pct': 'NULL'
    })

    if not os.path.exists(csv_path):
        # We explicitly support missing tier CSVs, returning 'NULL'
        return metrics

    core_utils = []
    mem_utils = []
    
    first_ts = None
    
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['timestamp_ns'] == 'timestamp_ns':
                continue
            ts = float(row['timestamp_ns'])
            if first_ts is None:
                first_ts = ts
                
            if (ts - first_ts) < (warmup_seconds * 1e9):
                continue
                
            core_utils.append(float(row['core_utilization_pct']))
            
            mem_used = float(row.get('memory_used_bytes', 0))
            mem_cap = float(row.get('memory_capacity_bytes', 0))
            
            if mem_cap > 0:
                mem_utils.append((mem_used / mem_cap) * 100.0)
            else:
                mem_utils.append(0.0)

    print(f"Loaded {len(core_utils)} valid samples from {os.path.basename(csv_path)} (after warm-up filter)")

    if not core_utils:
        return metrics

    metrics[f'{prefix}_utilization_pct'] = statistics.mean(core_utils)
    metrics[f'{prefix}_utilization_peak_pct'] = max(core_utils)
    
    metrics[f'{mem_prefix}_utilization_pct'] = statistics.mean(mem_utils)
    metrics[f'{mem_prefix}_utilization_peak_pct'] = max(mem_utils)
    
    return metrics

def main():
    args = parse_args()
    
    # Base paths
    ops_path = os.path.join(args.run_dir, "hcop_operations.csv")
    dpu_path = os.path.join(args.run_dir, "hcop_utilization_dpu.csv")
    switch_path = os.path.join(args.run_dir, "hcop_utilization_switch.csv")
    host_path = os.path.join(args.run_dir, "hcop_utilization_host.csv")
    
    metrics = {}
    
    # Parse Operations
    op_metrics = analyze_operations(ops_path, args.warmup_seconds)
    metrics.update(op_metrics)
    
    if args.placement != "unknown":
        metrics['placement_config'] = args.placement
    if args.primitive != "unknown":
        metrics['primitive_type'] = args.primitive
    if args.workload != "unknown":
        metrics['workload_params'] = args.workload
        
    # Parse Utilization Tiers
    dpu_metrics = analyze_utilization(dpu_path, args.warmup_seconds, 'dpu_core')
    switch_metrics = analyze_utilization(switch_path, args.warmup_seconds, 'switch')
    host_metrics = analyze_utilization(host_path, args.warmup_seconds, 'host_cpu')
    
    metrics.update(dpu_metrics)
    
    # Switch stage utilization should be the switch's memory utilization (SRAM usage)
    metrics['switch_stage_utilization_pct'] = switch_metrics.get('switch_memory_utilization_pct', 'NULL')
    metrics['switch_stage_utilization_peak_pct'] = switch_metrics.get('switch_memory_utilization_peak_pct', 'NULL')
    
    metrics.update(host_metrics)
    
    # We don't necessarily care about logging switch memory as requested, just maintaining the target schema
    # The output columns requested:
    output_cols = [
        'placement_config', 'primitive_type', 'workload_params',
        'latency_mean_ns', 'latency_p50_ns', 'latency_p99_ns',
        'throughput_ops_per_sec',
        'host_cpu_utilization_pct', 'host_cpu_utilization_peak_pct',
        'switch_stage_utilization_pct', 'switch_stage_utilization_peak_pct',
        'dpu_core_utilization_pct', 'dpu_core_utilization_peak_pct',
        'dpu_memory_utilization_pct', 'dpu_memory_utilization_peak_pct',
        'exception_rate_pct', 'overflow_rate_pct'
    ]
    
    with open(args.out, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=output_cols, extrasaction='ignore')
        writer.writeheader()
        writer.writerow(metrics)
        
if __name__ == "__main__":
    main()
