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

run_single() {
    local PRIMITIVE=$1
    local PLACEMENT=$2
    local HIGH_LOAD_DIR="${RESULTS}/${PRIMITIVE}_highload"
    
    mkdir -p "$HIGH_LOAD_DIR"
    export HCOP_PRIMITIVE="$PRIMITIVE"
    export HCOP_PLACEMENT="$PLACEMENT"
    
    echo "================================================================"
    echo "  PRIMITIVE: $PRIMITIVE | PLACEMENT: $PLACEMENT (d=0, n=500, 15m timeout)"
    echo "================================================================"
    
    pkill -9 -f qemu-system-x86 || true
    pkill -9 -f i40e_bm || true
    pkill -9 -f hcop_switch || true
    pkill -9 -f dpu_bm || true
    sleep 2

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
    rm -rf "$SIM_DIR"

    echo "  Running simulation..."
    local LOG_FILE="$WORKDIR/run_${PRIMITIVE}_${PLACEMENT}_highload.log"
    
    # Launch simulation in background
    timeout -s SIGINT 900 "$SIMBRICKS_RUN" \
        --repo "$REPO/" \
        --workdir "$WORKDIR/" \
        --verbose --force \
        "$TOPO" > "$LOG_FILE" 2>&1 &
        
    local SIM_PID=$!
    
    # Monitor log file for completion of op 499
    # We will check every 5 seconds.
    local count=0
    while kill -0 $SIM_PID 2>/dev/null; do
        if grep -q "] 499," "$LOG_FILE" 2>/dev/null; then
            echo "  Detected operation 499 finished! Sending SIGINT for early graceful shutdown..."
            kill -SIGINT $SIM_PID
            wait $SIM_PID || true
            break
        fi
        sleep 5
        count=$((count+5))
        if [ $count -ge 900 ]; then
            break
        fi
    done
    wait $SIM_PID || true

    # Find standard run dir
    local RUN_DIR=$(ls -td "${SIM_DIR}"/[0-9]* 2>/dev/null | head -1)

    if [ -z "$RUN_DIR" ]; then
        echo "  ERROR: No RUN_DIR found for $PRIMITIVE $PLACEMENT"
        return
    fi
    
    echo "  Collecting and aggregating CSVs..."
    python3 scripts/collect_hcop_csvs.py \
        --run-dir "$RUN_DIR" \
        --primitive "$PRIMITIVE" \
        --placement "$PLACEMENT" \
        --results-dir "$HIGH_LOAD_DIR" \
        --warmup-seconds 0 > "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1 || true
    
    OUT_DIR="${HIGH_LOAD_DIR}/${PRIMITIVE}/${PLACEMENT}"
    python3 scripts/aggregate_telemetry.py \
        --run-dir "$OUT_DIR" \
        --out "${OUT_DIR}/summary.csv" \
        --warmup-seconds 0 \
        --placement "$PLACEMENT" \
        --primitive "$PRIMITIVE" \
        --workload "n=500,d=0" >> "$WORKDIR/collect_${PRIMITIVE}_${PLACEMENT}_highload.log" 2>&1 || true
        
    echo "  Done with $PRIMITIVE $PLACEMENT."
    if [ -f "${OUT_DIR}/summary.csv" ]; then
        cat "${OUT_DIR}/summary.csv"
    fi
    echo ""
}

# PAXOS (4 remaining)
run_single "paxos" "dpu_only"
run_single "paxos" "switch_host"
run_single "paxos" "switch_dpu"
run_single "paxos" "dpu_host"

# LOCK (5 placements)
run_single "lock" "switch_only"
run_single "lock" "dpu_only"
run_single "lock" "switch_host"
run_single "lock" "switch_dpu"
run_single "lock" "dpu_host"

echo "All remaining high load sweeps completed."
