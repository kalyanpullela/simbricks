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
    echo "=== Running ${PRIM}/${PLACE} ==="
    killall -9 qemu-system-x86_64 hcop_switch simbricks-run 2>/dev/null || true
    sleep 2
    rm -rf "$WDIR"
    HCOP_PLACEMENT="$PLACE" HCOP_PRIMITIVE="$PRIM" HCOP_NUM_OPS=100 \
    PYTHONPATH="$PYPATH" \
    timeout --kill-after=900s 900 \
    "$SIMBRICKS_RUN" --repo "$REPO" --workdir "$WDIR" --verbose --force "$TOPO" \
    > "${WDIR}.log" 2>&1
    echo "  exit=$?"
    local RD
    RD=$(find "$WDIR" -name "out.json" -path "*/output/*" | head -1 | sed 's|/output/out.json||')
    if [ -n "$RD" ]; then
        HCOP_NUM_OPS=100 python3 "$COLLECT" --run-dir "$RD" --primitive "$PRIM" --placement "$PLACE" --results-dir results
    else
        echo "  ERROR: no out.json found"
    fi
}

run_one barrier host_only experiments/out/br_ho_v2
run_one barrier switch_host experiments/out/br_sh_v2
run_one barrier dpu_host experiments/out/br_dh_v2
echo "=== ALL DONE ==="
