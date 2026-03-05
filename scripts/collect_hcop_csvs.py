#!/usr/bin/env python3
"""
Post-run CSV collection script for HCOP experiments.

Extracts CSV data from simulation output (out.json) using the
--- BEGIN/END delimiters, then runs aggregate_telemetry.py and
produces a summary CSV in the results directory.

Usage:
    python3 scripts/collect_hcop_csvs.py \
        --run-dir experiments/out/simulation-hcop_paxos_switch_only/29 \
        --primitive paxos \
        --placement switch_only \
        --results-dir results
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys


def extract_csv_block(lines, begin_marker, end_marker):
    """Extract lines between BEGIN/END markers."""
    in_block = False
    rows = []
    for line in lines:
        line = line.strip()
        if line == begin_marker:
            in_block = True
            continue
        if line == end_marker:
            in_block = False
            continue
        if in_block and len(line) > 0:
            rows.append(line)
    return rows


def extract_from_json(json_path, primitive=None, placement=None):
    """Extract CSV data from out.json using delimiters.
    
    For barrier host_only and switch_host, operations are extracted only
    from host0 (RunClient). Host1 runs in combined/dual mode where the
    coordinator's latency (~10µs) measures only local inject time, not
    the full round-trip. Host0's RunClient captures the real latency.
    """
    with open(json_path) as f:
        data = json.load(f)

    ops_rows = []
    util_rows = []

    # For barrier + host1_is_server placements, only extract ops from host0
    host1_is_coordinator = (
        primitive == "barrier" and
        placement in ("host_only", "switch_host", "dpu_host", "switch_dpu_host")
    )

    for host_key in ['host.host0', 'host.host1']:
        host_data = data.get(host_key, {})
        for output_block in host_data.get('output', []):
            stdout_lines = output_block.get('stdout', [])
            # Each line is a string in the array, strip \r
            cleaned = [line.rstrip('\r\n') for line in stdout_lines]

            ops_rows += extract_csv_block(
                cleaned,
                "--- BEGIN hcop_operations.csv ---",
                "--- END hcop_operations.csv ---"
            )

            # Always extract utilization from all hosts
            util_rows += extract_csv_block(
                cleaned,
                "--- BEGIN hcop_utilization_host.csv ---",
                "--- END hcop_utilization_host.csv ---"
            )

    return ops_rows, util_rows


def deduplicate_csv_rows(rows):
    """Remove duplicate rows, keeping header first."""
    if not rows:
        return rows
    
    # Find header (first row that looks like a header)
    header = None
    data_rows = []
    seen = set()
    for row in rows:
        if 'operation_id' in row or 'timestamp_ns' in row:
            if header is None:
                header = row
            continue  # Skip all header rows
        if row not in seen:
            seen.add(row)
            data_rows.append(row)
    
    result = []
    if header:
        result.append(header)
    result.extend(data_rows)
    return result


def main():
    parser = argparse.ArgumentParser(description="Collect HCOP CSVs from simulation run")
    parser.add_argument("--run-dir", required=True, help="Path to the run directory")
    parser.add_argument("--primitive", required=True, help="Primitive type (paxos/lock/barrier)")
    parser.add_argument("--placement", required=True, help="Placement config (switch_only, etc.)")
    parser.add_argument("--results-dir", default="results", help="Base results directory")
    parser.add_argument("--warmup-seconds", type=float, default=0.5, help="Warm-up filter seconds")
    args = parser.parse_args()

    json_path = os.path.join(args.run_dir, "output", "out.json")
    if not os.path.exists(json_path):
        print(f"ERROR: {json_path} not found")
        sys.exit(1)

    print(f"Extracting CSVs from {json_path}...")
    ops_rows, util_rows = extract_from_json(json_path, primitive=args.primitive, placement=args.placement)
    
    ops_rows = deduplicate_csv_rows(ops_rows)
    util_rows = deduplicate_csv_rows(util_rows)
    
    print(f"  Operations rows: {len(ops_rows)}")
    print(f"  Utilization rows: {len(util_rows)}")

    # Create output directory
    out_dir = os.path.join(args.results_dir, args.primitive, args.placement)
    os.makedirs(out_dir, exist_ok=True)

    # Write extracted CSVs
    ops_csv = os.path.join(out_dir, "hcop_operations.csv")
    util_csv = os.path.join(out_dir, "hcop_utilization_host.csv")

    with open(ops_csv, 'w') as f:
        f.write('\n'.join(ops_rows) + '\n')
    with open(util_csv, 'w') as f:
        f.write('\n'.join(util_rows) + '\n')

    print(f"  Written: {ops_csv}")
    print(f"  Written: {util_csv}")

    # Copy switch/DPU utilization CSVs from /tmp/ where simulator
    # processes write them directly (switch and DPU run as host processes,
    # not inside QEMU guests).
    # IMPORTANT: Only copy if the placement actually uses that component,
    # otherwise stale files from a previous run leak into the results.
    import shutil
    placements_with_switch = {"switch_only", "switch_dpu", "switch_host", "switch_dpu_host"}
    placements_with_dpu = {"dpu_only", "switch_dpu", "dpu_host", "switch_dpu_host"}

    csv_component_map = {
        "hcop_utilization_switch.csv": placements_with_switch,
        "hcop_utilization_dpu.csv": placements_with_dpu,
    }

    for csv_name, valid_placements in csv_component_map.items():
        if args.placement not in valid_placements:
            print(f"  Skipping {csv_name} (placement '{args.placement}' has no {csv_name.split('_')[-1].split('.')[0]})")
            dest = os.path.join(out_dir, csv_name)
            if os.path.exists(dest):
                os.remove(dest)
                print(f"  Deleted stale {csv_name} from {out_dir}")
            continue
        tmp_path = os.path.join("/tmp", csv_name)
        if os.path.exists(tmp_path) and os.path.getsize(tmp_path) > 0:
            shutil.copy2(tmp_path, os.path.join(out_dir, csv_name))
            print(f"  Copied {csv_name} from /tmp/ ({os.path.getsize(tmp_path)} bytes)")

    # Also search the output directory tree as a fallback
    output_dir = os.path.join(args.run_dir, "output")
    for dirpath, dirnames, filenames in os.walk(output_dir):
        for fn in filenames:
            if fn in csv_component_map:
                if args.placement not in csv_component_map[fn]:
                    continue
                dest = os.path.join(out_dir, fn)
                if not os.path.exists(dest):
                    shutil.copy2(os.path.join(dirpath, fn), dest)
                    print(f"  Copied {fn} from output dir")

    # Run aggregate_telemetry.py
    summary_csv = os.path.join(out_dir, "summary.csv")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    agg_script = os.path.join(script_dir, "aggregate_telemetry.py")

    if os.path.exists(agg_script):
        cmd = [
            sys.executable, agg_script,
            "--run-dir", out_dir,
            "--out", summary_csv,
            "--warmup-seconds", str(args.warmup_seconds),
            "--placement", args.placement,
            "--primitive", args.primitive,
            "--workload", f"n={os.environ.get('HCOP_NUM_OPS', '100')}",
        ]
        print(f"Running aggregation: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            print(f"  Summary written: {summary_csv}")
            # Print summary content
            if os.path.exists(summary_csv):
                print("\n=== SUMMARY CSV ===")
                with open(summary_csv) as f:
                    print(f.read())
        else:
            print(f"  Aggregation failed (rc={result.returncode})")
            print(f"  stdout: {result.stdout}")
            print(f"  stderr: {result.stderr}")
    else:
        print(f"  WARNING: {agg_script} not found, skipping aggregation")

    print("Done.")


if __name__ == "__main__":
    main()
