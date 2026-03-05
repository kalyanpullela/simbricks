#!/bin/bash
# Run all remaining Paxos placements and collect results.
# Usage: bash scripts/run_paxos_sweep.sh
set -eo pipefail

REPO="/home/user/Documents/simbricks"
SIMBRICKS_RUN="/home/user/.local/bin/simbricks-run"
WORKDIR="experiments/out"
TOPO="experiments/pyexps/hcop/hcop_topology.py"
RESULTS="results"
PRIMITIVE="paxos"
NUM_OPS=100

cd "$REPO"
export PYTHONPATH="$REPO/symphony/"
export HCOP_PRIMITIVE="$PRIMITIVE"
export HCOP_NUM_OPS="$NUM_OPS"

PLACEMENTS="switch_dpu dpu_host switch_dpu_host switch_host host_only"

for PLACEMENT in $PLACEMENTS; do
    echo ""
    echo "================================================================"
    echo "  PLACEMENT: $PLACEMENT — $(date)"
    echo "================================================================"
    
    export HCOP_PLACEMENT="$PLACEMENT"
    SIM_NAME="simulation-hcop_${PRIMITIVE}_${PLACEMENT}"
    
    # Clean stale /tmp CSVs
    rm -f /tmp/hcop_utilization_switch.csv /tmp/hcop_utilization_dpu.csv
    
    # Clean old output
    rm -rf "$WORKDIR/$SIM_NAME"
    
    echo "  Running simulation..."
    if timeout 600 "$SIMBRICKS_RUN" \
        --repo "$REPO/" \
        --workdir "$WORKDIR/" \
        --verbose --force \
        "$TOPO" 2>&1 | tail -20; then
        echo "  Simulation completed."
    else
        echo "  ERROR: Simulation failed or timed out for $PLACEMENT"
        echo "  SKIPPING to next placement."
        continue
    fi
    
    # Find the run directory (contains the numeric ID subdirectory)
    RUN_DIR=$(find "$WORKDIR/$SIM_NAME" -name "out.json" -path "*/output/out.json" 2>/dev/null | head -1 | xargs -I{} dirname {} | xargs -I{} dirname {})
    
    if [ -z "$RUN_DIR" ]; then
        echo "  ERROR: Could not find out.json for $PLACEMENT"
        continue
    fi
    
    echo "  Run dir: $RUN_DIR"
    echo "  Collecting CSVs..."
    
    python3 scripts/collect_hcop_csvs.py \
        --run-dir "$RUN_DIR" \
        --primitive "$PRIMITIVE" \
        --placement "$PLACEMENT" \
        --results-dir "$RESULTS" 2>&1
    
    echo "  Done with $PLACEMENT"
    echo ""
done

echo ""
echo "================================================================"
echo "  ALL PLACEMENTS COMPLETE — SUMMARY COMPARISON"
echo "================================================================"
echo ""

# Print comparison table
printf "%-20s" "placement"
head -1 "$RESULTS/$PRIMITIVE/switch_only/summary.csv" 2>/dev/null | tr ',' '\n' | while read h; do
    printf " | %-20s" "$h"
done
echo ""

for p in switch_only dpu_only switch_dpu dpu_host switch_dpu_host switch_host host_only; do
    CSV="$RESULTS/$PRIMITIVE/$p/summary.csv"
    if [ -f "$CSV" ]; then
        printf "%-20s" "$p"
        tail -1 "$CSV" | tr ',' '\n' | while read v; do
            printf " | %-20s" "$v"
        done
        echo ""
    else
        printf "%-20s | MISSING\n" "$p"
    fi
done
