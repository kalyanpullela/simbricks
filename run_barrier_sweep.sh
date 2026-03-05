#!/bin/bash
set -e
cd /home/user/Documents/simbricks

REPO=/home/user/Documents/simbricks
PYPATH="/home/user/Documents/simbricks/symphony/"
SIMBRICKS_RUN=/home/user/.local/bin/simbricks-run
TOPO=experiments/pyexps/hcop/hcop_topology.py
COLLECT=scripts/collect_hcop_csvs.py
TIMEOUT=900  # 15 minutes
NUM_OPS=100

run_config() {
    local PRIM=$1
    local PLACE=$2
    local LABEL="${PRIM}/${PLACE}"
    local WORKDIR="experiments/out/${PRIM}_${PLACE}"
    
    echo ""
    echo "========================================"
    echo "  RUNNING: ${LABEL}  (n=${NUM_OPS})"
    echo "========================================"
    
    # Kill orphans
    killall -9 qemu-system-x86_64 hcop_switch simbricks-run 2>/dev/null || true
    sleep 2
    
    # Clean workdir
    rm -rf "$WORKDIR"
    
    # Run simulation
    HCOP_PLACEMENT="$PLACE" \
    HCOP_PRIMITIVE="$PRIM" \
    HCOP_NUM_OPS="$NUM_OPS" \
    PYTHONPATH="$PYPATH" \
    timeout --kill-after="${TIMEOUT}s" "${TIMEOUT}" \
      "$SIMBRICKS_RUN" \
        --repo "$REPO" \
        --workdir "$WORKDIR" \
        --verbose --force \
        "$TOPO" > "${WORKDIR}.log" 2>&1
    
    local RC=$?
    if [ $RC -ne 0 ]; then
        echo "  *** ${LABEL} FAILED (exit=$RC) ***"
        return $RC
    fi
    
    echo "  ${LABEL} completed successfully"
    
    # Find the run directory (contains output/out.json)
    local RUN_DIR=$(find "$WORKDIR" -name "out.json" -path "*/output/*" | head -1 | sed 's|/output/out.json||')
    if [ -z "$RUN_DIR" ]; then
        echo "  *** No out.json found for ${LABEL} ***"
        return 1
    fi
    
    # Collect CSVs and aggregate
    HCOP_NUM_OPS="$NUM_OPS" \
    python3 "$COLLECT" \
      --run-dir "$RUN_DIR" \
      --primitive "$PRIM" \
      --placement "$PLACE" \
      --results-dir results
    
    echo "  ${LABEL} collection done"
    return 0
}

# ---- STEP 1: All barrier configs ----
BARRIER_CONFIGS="switch_host host_only dpu_host switch_only dpu_only switch_dpu switch_dpu_host"
for place in $BARRIER_CONFIGS; do
    run_config barrier "$place" || echo "SKIPPED: barrier/$place"
done

# ---- STEP 2: Re-run switch_dpu_host for paxos and lock ----
for prim in paxos lock; do
    run_config "$prim" switch_dpu_host || echo "SKIPPED: $prim/switch_dpu_host"
done

echo ""
echo "========================================"
echo "  ALL RUNS COMPLETE"
echo "========================================"
