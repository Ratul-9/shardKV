#!/bin/bash
# One command for every measurement in docs/BENCHMARKS.md that runs on one Linux machine.
# Takes a few hours. Run on a quiet machine (close the browser), from the project root:
#   RESULTS_DIR=bench/results/linux PIN_CPUS=0,1,2,3,4,5 WAL_DIR=$HOME/walbench scripts/reproduce_all.sh
# Then: scripts/summarize_results.py bench/results/linux/<file>
set -eu
cmake --preset release >/dev/null
cmake --build --preset release -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
export RESULTS_DIR=${RESULTS_DIR:-bench/results/linux}
mkdir -p "$RESULTS_DIR"
{ uname -a; lscpu 2>/dev/null | head -20; free -h 2>/dev/null;
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null;
  cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null; } > "$RESULTS_DIR/machine.txt"
scripts/bench_inprocess.sh 3 all
scripts/bench_sharded.sh 3
scripts/bench_table.sh 3
scripts/bench_phase6_9.sh 3
scripts/bench_cluster.sh 3
RUNS=20 scripts/chaos_local.sh 60
echo "all results in $RESULTS_DIR"
