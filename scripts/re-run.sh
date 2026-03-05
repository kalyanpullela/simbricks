#!/bin/bash
set -eo pipefail

REPO="/home/user/Documents/simbricks"
SIMBRICKS_RUN="/home/user/.local/bin/simbricks-run"
WORKDIR="experiments/out"
TOPO="experiments/pyexps/hcop/hcop_topology.py"
RESULTS="results"

cd "$REPO"
export PYTHONPATH="$REPO/symphony/"
export HCOP_PRIMITIVE="barrier"
export HCOP_NUM_OPS="100"

# Rebuild the host images by copying the updated binaries into them if necessary,
# but usually `make -C images/hcop` is enough if the guest image mounts it or we just need the binary.
# Actually, SimBricks generic app or raw command app might just execute /usr/local/bin/...
# usually `make -C images` builds the qcow2. Let's just run make -C images just in case
make -C images || true

for PLACEMENT in dpu_host host_only switch_host; do
    echo "================================================================"
    echo "  PLACEMENT: $PLACEMENT — $(date)"
    echo "================================================================"
    
    # Clean orphaned processes from previous runs
    pkill -9 -f qemu-system-x86 || true
    pkill -9 -f i40e_bm || true
    pkill -9 -f hcop_switch || true
    pkill -9 -f dpu_bm || true
    sleep 2

    export HCOP_PLACEMENT="$PLACEMENT"
    SIM_NAME="simulation-hcop_barrier_${PLACEMENT}"
    
    # Clean stale /tmp CSVs
    rm -f /tmp/hcop_utilization_switch.csv /tmp/hcop_utilization_dpu.csv
    rm -f /tmp/hcop_utilization_host.csv /tmp/hcop_operations.csv
    
    # Clean old output
    rm -rf "$WORKDIR"/simulation-hcop_br_*
    
    echo "  Running simulation..."
    if timeout 1200 "$SIMBRICKS_RUN" \
        --repo "$REPO/" \
        --workdir "$WORKDIR/" \
        --verbose --force \
        "$TOPO" > "$WORKDIR/run_${PLACEMENT}.log" 2>&1; then
        echo "  Simulation completed."
    else
        echo "  ERROR: Simulation failed or timed out for $PLACEMENT"
        continue
    fi
    
    RUN_DIR=$(ls -td "$WORKDIR"/simulation-hcop_br_*/[0-9]* | head -1)
    
    if [ ! -f "$RUN_DIR/output/out.json" ]; then
        echo "  ERROR: Could not find out.json in $RUN_DIR for $PLACEMENT"
        continue
    fi
    
    echo "  Collecting CSVs..."
    python3 scripts/collect_hcop_csvs.py \
        --run-dir "$RUN_DIR" \
        --primitive "barrier" \
        --placement "$PLACEMENT" \
        --results-dir "$RESULTS" 2>&1
done

echo "Regenerating master_summary.csv..."
python3 scripts/generate_master_summary.py
