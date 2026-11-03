#!/bin/bash
# Phase 2 lock-contention matrix, in process (no network). Run from the project root, release build.
#   scripts/bench_inprocess.sh [reps=3] [mode=all|closed|open]
# Writes bench/results/phase2_inprocess_closed.txt (throughput) and phase2_inprocess_open.txt (latency).
set -u
BIN=build/release/bench/contention
REPS=${1:-3}
MODE=${2:-all}
OUT=bench/results
[ -x "$BIN" ] || { echo "build first: cmake --build --preset release"; exit 1; }
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS"; uptime; } > "$1"; }

if [ "$MODE" = all ] || [ "$MODE" = closed ]; then
  F=$OUT/phase2_inprocess_closed.txt; header $F
  # Closed loop: maximum throughput, 90% Get / 10% Put, 4 s measured after 1 s warm-up.
  for keys in 1000000 1000; do
    for rep in $(seq 1 "$REPS"); do
      echo "## store=none clients=1 keys=$keys rep=$rep" >> $F
      $BIN --store=none --clients=1 --mode=closed --keys=$keys --seconds=4 --warmup=1 >> $F
      for store in mutex shared; do
        for c in 1 4 16 64; do
          echo "## store=$store clients=$c keys=$keys rep=$rep" >> $F
          $BIN --store=$store --clients=$c --mode=closed --keys=$keys --seconds=4 --warmup=1 >> $F
        done
      done
    done
  done
  uptime >> $F
fi

if [ "$MODE" = all ] || [ "$MODE" = open ]; then
  F=$OUT/phase2_inprocess_open.txt; header $F
  # Open loop: latency percentiles at a fixed total offered rate, 1M keys, 6 s measured after 1 s warm-up.
  for rate in 200000 2000000; do
    echo "## store=none clients=1 rate=$rate" >> $F
    $BIN --store=none --clients=1 --mode=open --rate=$rate --keys=1000000 --seconds=6 --warmup=1 >> $F
    for store in mutex shared; do
      for c in 1 4 16 64; do
        echo "## store=$store clients=$c rate=$rate" >> $F
        $BIN --store=$store --clients=$c --mode=open --rate=$rate --keys=1000000 --seconds=6 --warmup=1 >> $F
      done
    done
  done
  uptime >> $F
fi
echo done
