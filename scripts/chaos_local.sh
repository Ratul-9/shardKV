#!/bin/bash
# Phase 10 on a real (local) cluster: record a client history while crashing nodes with kill -9,
# then check it for linearizability. Repeat it with RUNS=N for many runs.
#   scripts/chaos_local.sh [seconds=60]
# Environment: RUNS (default 1), RESULTS_DIR (default bench/results/chaos), KILL_EVERY (s, default 5)
# Linux-only partitions (root, iptables): scripts/chaos_partition_linux.sh.
set -u
SECONDS_PER_RUN=${1:-60}
RUNS=${RUNS:-1}
KILL_EVERY=${KILL_EVERY:-5}
OUT=${RESULTS_DIR:-bench/results/chaos}
BIN=build/release/apps
SERVERS=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003
mkdir -p "$OUT"
fails=0
for run in $(seq 1 "$RUNS"); do
  scripts/cluster_local.sh wipe >/dev/null 2>&1
  scripts/cluster_local.sh start >/dev/null
  H="$OUT/history-$(date +%Y%m%d-%H%M%S)-$run.txt"
  "$BIN/kvctl" --servers=$SERVERS --attempt-timeout-ms=500 --timeout-ms=5000 history \
      --out="$H" --clients=4 --seconds="$SECONDS_PER_RUN" --keys=5 &
  client=$!
  end=$(( $(date +%s) + SECONDS_PER_RUN ))
  while [ "$(date +%s)" -lt "$end" ]; do
    sleep "$KILL_EVERY"
    victim=$(( (RANDOM % 3) + 1 ))
    scripts/cluster_local.sh kill $victim
    sleep $(( (RANDOM % 3) + 1 ))
    scripts/cluster_local.sh restart $victim
  done
  wait $client
  scripts/cluster_local.sh stop >/dev/null
  if "$BIN/lincheck" "$H"; then :; else fails=$((fails + 1)); echo "FAILED: $H (keep it: it reproduces the bug)"; fi
done
echo "chaos: $RUNS runs, $fails not linearizable"
[ "$fails" -eq 0 ]
