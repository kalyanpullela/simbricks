#!/bin/bash
set -e
cd /home/user/Documents/simbricks
REPO=/home/user/Documents/simbricks
PYPATH="/home/user/Documents/simbricks/symphony/"
SIMBRICKS_RUN=/home/user/.local/bin/simbricks-run
TOPO=experiments/pyexps/hcop/hcop_topology.py
COLLECT=scripts/collect_hcop_csvs.py

run_one() {
    local PRIM=$1 PLACE=$2 WDIR=$3

    echo ""
    echo "========================================"
    echo "  RUNNING: ${PRIM}/${PLACE}"
    echo "========================================"

    # Clean up any orphaned processes
    killall -9 qemu-system-x86_64 hcop_switch simbricks-run 2>/dev/null || true
    sleep 2

    # CRITICAL: Remove stale /tmp CSVs to prevent contamination (Issue 3)
    rm -f /tmp/hcop_utilization_switch.csv /tmp/hcop_utilization_dpu.csv /tmp/hcop_operations.csv /tmp/hcop_utilization_host.csv

    rm -rf "$WDIR"
    HCOP_PLACEMENT="$PLACE" HCOP_PRIMITIVE="$PRIM" HCOP_NUM_OPS=100 \
    PYTHONPATH="$PYPATH" \
    timeout --kill-after=900s 900 \
    "$SIMBRICKS_RUN" --repo "$REPO" --workdir "$WDIR" --verbose --force "$TOPO" \
    > "${WDIR}.log" 2>&1
    local EC=$?
    echo "  exit=$EC"
    if [ $EC -ne 0 ]; then
        echo "  WARNING: Non-zero exit code $EC"
    fi

    local RD
    RD=$(find "$WDIR" -name "out.json" -path "*/output/*" | head -1 | sed 's|/output/out.json||')
    if [ -n "$RD" ]; then
        echo "  run_dir=$RD"
        # Also remove stale result CSVs before collection
        rm -f "results/${PRIM}/${PLACE}/hcop_utilization_dpu.csv" \
              "results/${PRIM}/${PLACE}/hcop_utilization_switch.csv"
        HCOP_NUM_OPS=100 python3 "$COLLECT" \
            --run-dir "$RD" \
            --primitive "$PRIM" \
            --placement "$PLACE" \
            --results-dir results
    else
        echo "  ERROR: no out.json found"
    fi
}

# Re-run ALL 7 barrier configs for consistent data
run_one barrier host_only      experiments/out/final_br_ho
run_one barrier switch_host    experiments/out/final_br_sh
run_one barrier switch_only    experiments/out/final_br_sw
run_one barrier dpu_only       experiments/out/final_br_dp
run_one barrier switch_dpu     experiments/out/final_br_sd
run_one barrier dpu_host       experiments/out/final_br_dh
run_one barrier switch_dpu_host experiments/out/final_br_sdh

echo ""
echo "========================================"
echo "  ALL BARRIER RUNS COMPLETE"
echo "========================================"
