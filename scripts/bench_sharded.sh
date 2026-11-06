#!/bin/bash
# Phase 3 sharding matrix, in process, on Linux. Run from the project root, release build.
#   scripts/bench_sharded.sh [reps=3]
# Environment:
#   RESULTS_DIR   where results go (default bench/results/linux)
#   PIN_CPUS      CPUs to pin shards to, one per physical core, e.g. "0,1,2,3,4,5". Check
#                 `lscpu -e=CPU,CORE` first: CPUs on the SAME core are SMT siblings, so pick
#                 one CPU per distinct CORE. Set PIN_CPUS="" to run unpinned.
# Writes phase3_closed.txt (throughput) and phase3_open.txt (latency at a fixed offered rate).
set -u
BIN=build/release/bench/contention
REPS=${1:-3}
OUT=${RESULTS_DIR:-bench/results/linux}
PIN_CPUS=${PIN_CPUS-0,1,2,3,4,5}
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "build first: cmake --build --preset release"; exit 1; }
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS pin_cpus=$PIN_CPUS"; uptime; } > "$1"; }
PIN=""; [ -n "$PIN_CPUS" ] && PIN="--pin=$PIN_CPUS"

F=$OUT/phase3_closed.txt; header $F
# Closed loop: maximum throughput, 90% Get / 10% Put, 1M keys, 4 s measured after 1 s warm-up.
# Baselines on the same machine first, so the comparison is like for like.
for rep in $(seq 1 "$REPS"); do
  echo "## store=none clients=1 rep=$rep" >> $F
  $BIN --store=none --clients=1 --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $F
  for c in 1 4 12 64; do
    echo "## store=mutex clients=$c rep=$rep" >> $F
    $BIN --store=mutex --clients=$c --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $F
  done
  for shards in 1 2 3 4 5 6; do
    for c in 1 4 12 64; do
      echo "## store=sharded shards=$shards clients=$c rep=$rep" >> $F
      $BIN --store=sharded --shards=$shards $PIN --clients=$c --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $F
    done
  done
done
uptime >> $F

F=$OUT/phase3_open.txt; header $F
# Open loop: latency at a fixed total offered rate (well below capacity), 6 s measured after 1 s.
for rate in 50000 200000; do
  for shards in 1 2 4 6; do
    for c in 4 12; do
      echo "## store=sharded shards=$shards clients=$c rate=$rate" >> $F
      $BIN --store=sharded --shards=$shards $PIN --clients=$c --mode=open --rate=$rate --keys=1000000 --seconds=6 --warmup=1 >> $F
    done
  done
  for c in 4 12; do
    echo "## store=mutex clients=$c rate=$rate" >> $F
    $BIN --store=mutex --clients=$c --mode=open --rate=$rate --keys=1000000 --seconds=6 --warmup=1 >> $F
  done
done
uptime >> $F
echo done
