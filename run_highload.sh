#!/bin/bash
set -eo pipefail

REPO="/home/user/Documents/simbricks"
SIMBRICKS_RUN="/home/user/.local/bin/simbricks-run"
WORKDIR="experiments/out"
TOPO="experiments/pyexps/hcop/hcop_topology.py"
RESULTS="results"

cd "$REPO"
export PYTHONPATH="$REPO/symphony/"
export HCOP_DELAY_US=0
export HCOP_NUM_OPS=500

PLACEMENTS="switch_only switch_host switch_dpu dpu_only dpu_host"

run_single() {
    local PRIMITIVE=$1
    local PLACEMENT=$2
    local HIGH_LOAD_DIR="${RESULTS}/${PRIMITIVE}_highload"
    
    mkdir -p "$HIGH_LOAD_DIR"
    export HCOP_PRIMITIVE="$PRIMITIVE"
    export HCOP_PLACEMENT="$PLACEMENT"
    
    echo "================================================================"
    echo "  STARTING: $PRIMITIVE | $PLACEMENT (d=0, n=500)"
    echo "================================================================"
    
    # Clean old output for this placement/primitive combo
    rm -rf "$WORKDIR"/simulation-hcop_${PRIMITIVE}_${PLACEMENT}_highload*
    
    # Run the simulation
    # Add a unique suffix to the sim name using WORKDIR / hcop_topology overrides? No, SimBricks names it based on sys_b.name
    # Since each sys_b.name is unique (hcop_px_sw, etc), their output directories are unique!
    timeout 300 "$SIMBRICKS_RUN" \
        --repo "$REPO/" \
        --workdir "$WORKDIR/" \
        --verbose --force \
        "$TOPO" > "$WORKDIR/run_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1 || echo "  $PRIMITIVE $PLACEMENT reached timeout or failed, proceeding to collect."
        
    # Find the generated output dir for this specific simulation name
    # We must match the abbreviated name that hcop_topology.py generates.
    local SHORT_PRIM="px"
    [ "$PRIMITIVE" = "lock" ] && SHORT_PRIM="lk"
    local SHORT_PLACE=""
    case "$PLACEMENT" in
        "switch_only") SHORT_PLACE="sw" ;;
        "dpu_only") SHORT_PLACE="dp" ;;
        "switch_host") SHORT_PLACE="sh" ;;
        "switch_dpu") SHORT_PLACE="sd" ;;
        "dpu_host") SHORT_PLACE="dh" ;;
    esac
    
    local SIM_DIR="$WORKDIR/simulation-hcop_${SHORT_PRIM}_${SHORT_PLACE}"
    local RUN_DIR=$(ls -td "${SIM_DIR}"/[0-9]* 2>/dev/null | head -1)
        
        if [ -z "$RUN_DIR" ] || [ ! -f "$RUN_DIR/output/out.json" ]; then
            echo "  ERROR: Could not find out.json in $RUN_DIR for $PLACEMENT"
            continue
        fi
        
        echo "  Collecting and aggregating CSVs..."
        python3 scripts/collect_hcop_csvs.py \
            --run-dir "$RUN_DIR" \
            --primitive "$PRIMITIVE" \
            --placement "$PLACEMENT" \
            --results-dir "$HIGH_LOAD_DIR" \
            --warmup-seconds 0.5 > "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1
        
        # Re-run aggregation with different workload tag to mark as high-load
        echo "  Tagging summary as d=0..."
        OUT_DIR="${HIGH_LOAD_DIR}/${PRIMITIVE}/${PLACEMENT}"
        python3 scripts/aggregate_telemetry.py \
            --run-dir "$OUT_DIR" \
            --out "${OUT_DIR}/summary.csv" \
            --warmup-seconds 0.5 \
            --placement "$PLACEMENT" \
            --primitive "$PRIMITIVE" \
            --workload "n=500,d=0" >> "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1
            
    echo "  $PRIMITIVE $PLACEMENT Finished."
}

# Run all 10 configurations in parallel
for PLACEMENT in $PLACEMENTS; do
    run_single "paxos" "$PLACEMENT" &
done

for PLACEMENT in $PLACEMENTS; do
    run_single "lock" "$PLACEMENT" &
done

echo "Waiting for all high-load sweeps to finish (up to ~5 minutes)..."
wait

# Run Lock sweep


echo "High load sweeps completed."
