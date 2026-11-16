#!/bin/bash
# Phase 8/11 cluster measurements on THIS machine (all 3 nodes and the load generator share it,
# so these are single-host numbers; the EC2 version is in deploy/aws/README.md).
#   scripts/bench_cluster.sh [reps=3]
# Writes cluster_throughput.txt, cluster_latency.txt, cluster_failover.txt to RESULTS_DIR.
set -u
REPS=${1:-3}
OUT=${RESULTS_DIR:-bench/results/linux}
BIN=build/release/apps
SERVERS=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003
mkdir -p "$OUT"
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS host=$(uname -n)"; uptime; } > "$1"; }

for sync in data none; do
  F=$OUT/cluster_throughput_sync_$sync.txt; header $F
  scripts/cluster_local.sh wipe >/dev/null 2>&1; scripts/cluster_local.sh start --sync=$sync >/dev/null
  for rep in $(seq 1 "$REPS"); do
    for c in 1 4 16 64; do
      echo "## sync=$sync clients=$c rep=$rep" >> $F
      "$BIN/kvctl" --servers=$SERVERS bench --mode=closed --clients=$c --seconds=10 --warmup=2 --read-pct=50 >> $F
    done
  done
  scripts/cluster_local.sh stop >/dev/null
done

F=$OUT/cluster_latency.txt; header $F
scripts/cluster_local.sh wipe >/dev/null 2>&1; scripts/cluster_local.sh start >/dev/null
for rep in $(seq 1 "$REPS"); do
  for rate in 1000 5000 20000; do
    echo "## sync=data clients=16 rate=$rate rep=$rep" >> $F
    "$BIN/kvctl" --servers=$SERVERS bench --mode=open --clients=16 --rate=$rate --seconds=15 --warmup=3 --read-pct=50 >> $F
  done
done

# Failover: kill -9 the leader, then poll until a new leader answers; repeat.
F=$OUT/cluster_failover.txt; header $F
echo "# election timeout 15-30 ticks of 10 ms = 150-300 ms; heartbeat 30 ms" >> $F
for rep in $(seq 1 $((REPS * 5))); do
  leader=$(scripts/cluster_local.sh leader)
  [ -n "$leader" ] || { sleep 1; continue; }
  t0=$(date +%s%N)
  scripts/cluster_local.sh kill "$leader" >/dev/null
  while :; do
    l=$(scripts/cluster_local.sh leader)
    [ -n "$l" ] && [ "$l" != "$leader" ] && break
  done
  t1=$(date +%s%N)
  echo "rep=$rep killed=$leader new_leader=$l failover_ms=$(( (t1 - t0) / 1000000 ))" >> $F
  scripts/cluster_local.sh restart "$leader" >/dev/null
  sleep 2
done
scripts/cluster_local.sh stop >/dev/null
echo "results in $OUT"
