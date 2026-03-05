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

PLACEMENTS="switch_only dpu_only"

run_sweep() {
    local PRIMITIVE=$1
    local HIGH_LOAD_DIR="${RESULTS}/${PRIMITIVE}_highload"
    
    mkdir -p "$HIGH_LOAD_DIR"
    export HCOP_PRIMITIVE="$PRIMITIVE"
    
    for PLACEMENT in $PLACEMENTS; do
        echo "================================================================"
        echo "  PRIMITIVE: $PRIMITIVE | PLACEMENT: $PLACEMENT (d=0, n=500)"
        echo "================================================================"
        
        # Clean orphaned processes from previous runs
        pkill -9 -f qemu-system-x86 || true
        pkill -9 -f i40e_bm || true
        pkill -9 -f hcop_switch || true
        pkill -9 -f dpu_bm || true
        sleep 2

        export HCOP_PLACEMENT="$PLACEMENT"
        
        rm -rf "$WORKDIR"/simulation-hcop_*
        
        echo "  Running simulation..."
        timeout -s SIGINT 300 "$SIMBRICKS_RUN" \
            --repo "$REPO/" \
            --workdir "$WORKDIR/" \
            --verbose --force \
            "$TOPO" > "$WORKDIR/run_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1 || echo "Timeout or failure"
        
        # Extract outputs
        local SHORT_PRIM="px"
        local SHORT_PLACE=""
        case "$PLACEMENT" in
            "switch_only") SHORT_PLACE="sw" ;;
            "dpu_only") SHORT_PLACE="dp" ;;
        esac
        
        local SIM_DIR="$WORKDIR/simulation-hcop_${SHORT_PRIM}_${SHORT_PLACE}"
        local RUN_DIR=$(ls -td "${SIM_DIR}"/[0-9]* 2>/dev/null | head -1)
        
        echo "  Collecting and aggregating CSVs..."
        python3 scripts/collect_hcop_csvs.py \
            --run-dir "$RUN_DIR" \
            --primitive "$PRIMITIVE" \
            --placement "$PLACEMENT" \
            --results-dir "$HIGH_LOAD_DIR" \
            --warmup-seconds 0 > "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1
        
        # Re-run aggregation with different workload tag to mark as high-load
        OUT_DIR="${HIGH_LOAD_DIR}/${PRIMITIVE}/${PLACEMENT}"
        python3 scripts/aggregate_telemetry.py \
            --run-dir "$OUT_DIR" \
            --out "${OUT_DIR}/summary.csv" \
            --warmup-seconds 0 \
            --placement "$PLACEMENT" \
            --primitive "$PRIMITIVE" \
            --workload "n=500,d=0" >> "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1
            
        echo "  Summary written to ${OUT_DIR}/summary.csv"
        cat "${OUT_DIR}/summary.csv"
        echo ""
    done
}

run_sweep "paxos"
