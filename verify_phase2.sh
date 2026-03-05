#!/bin/bash
set -e

echo "1. Production build..."
cd . && make -j$(nproc) > /dev/null

echo "2. DPU binary check..."
ls -la sims/nic/dpu_bm/dpu_bm
file sims/nic/dpu_bm/dpu_bm

echo "3. Shared library tests..."
# Paxos
g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -I./lib \
  lib/hcop/tests/test_paxos_state.cc lib/hcop/paxos_state.cc \
  -o /tmp/test_paxos_state && /tmp/test_paxos_state
# Lock
g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -I./lib \
  lib/hcop/tests/test_lock_state.cc lib/hcop/lock_state.cc \
  -o /tmp/test_lock_state && /tmp/test_lock_state
# Barrier
g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -I./lib \
  lib/hcop/tests/test_barrier_state.cc lib/hcop/barrier_state.cc \
  -o /tmp/test_barrier_state && /tmp/test_barrier_state

echo "4. DPU Handler integration tests..."
# Common deps
DEPS="sims/nic/dpu_bm/dpu_bm.cc sims/nic/dpu_bm/dpu_config.cc \
sims/nic/dpu_bm/arm_core_pool.cc sims/nic/dpu_bm/dram_store.cc \
lib/simbricks/nicbm/libnicbm.a lib/simbricks/nicif/libnicif.a \
lib/simbricks/network/libnetwork.a lib/simbricks/pcie/libpcie.a \
lib/simbricks/base/libbase.a"
LIBS="-lboost_fiber -lboost_context -lpthread -lelf"
FLAGS="-std=gnu++17 -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -DDPU_BM_NO_MAIN -I./lib -Isims/nic/dpu_bm -iquote./"

# All handlers and their states are required because dpu_bm constructor registers them all
ALL_HANDLERS="sims/nic/dpu_bm/paxos_dpu_handler.cc lib/hcop/paxos_state.cc \
sims/nic/dpu_bm/lock_dpu_handler.cc lib/hcop/lock_state.cc \
sims/nic/dpu_bm/barrier_dpu_handler.cc lib/hcop/barrier_state.cc"

# Paxos Handler
g++ $FLAGS sims/nic/dpu_bm/tests/test_paxos_handler.cc \
  $ALL_HANDLERS $DEPS $LIBS -o /tmp/test_paxos_handler && /tmp/test_paxos_handler

# Lock Handler
g++ $FLAGS sims/nic/dpu_bm/tests/test_lock_handler.cc \
  $ALL_HANDLERS $DEPS $LIBS -o /tmp/test_lock_handler && /tmp/test_lock_handler

# Barrier Handler
g++ $FLAGS sims/nic/dpu_bm/tests/test_barrier_handler.cc \
  $ALL_HANDLERS $DEPS $LIBS -o /tmp/test_barrier_handler && /tmp/test_barrier_handler

echo "5. Pipeline Smoke Tests..."
# Note: dpu_bm.cc constructor now registers handlers, so we MUST link their implementations
g++ $FLAGS sims/nic/dpu_bm/tests/test_pipeline.cc \
  sims/nic/dpu_bm/paxos_dpu_handler.cc lib/hcop/paxos_state.cc \
  sims/nic/dpu_bm/lock_dpu_handler.cc lib/hcop/lock_state.cc \
  sims/nic/dpu_bm/barrier_dpu_handler.cc lib/hcop/barrier_state.cc \
  $DEPS $LIBS -o /tmp/test_pipeline && /tmp/test_pipeline

echo "6. Host binaries..."
ls -la images/hcop/
file images/hcop/paxos_host images/hcop/lock_host images/hcop/barrier_host

echo "=== VERIFICATION SUMMARY ==="
echo "Report: All tests passed."
