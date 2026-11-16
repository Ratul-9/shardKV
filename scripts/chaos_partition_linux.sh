#!/bin/bash
# Phase 10, Linux only, needs root: network PARTITIONS and packet loss/delay between local nodes.
# NOT RUN YET (written on the Mac). Read before running: it edits iptables and tc rules on `lo`
# and removes them at exit (trap). Run on a test machine, never on one that matters.
#   sudo -E scripts/chaos_partition_linux.sh [seconds=120]
# Each node's Raft port is 700<id>. Isolating node N = drop packets to and from its Raft port.
set -u
SECONDS_TOTAL=${1:-120}
OUT=${RESULTS_DIR:-bench/results/chaos}
BIN=build/release/apps
SERVERS=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003
[ "$(id -u)" -eq 0 ] || { echo "needs root (iptables, tc)"; exit 1; }
mkdir -p "$OUT"
cleanup() {
  for id in 1 2 3; do
    iptables -D INPUT -p tcp --dport 700$id -j DROP 2>/dev/null
    iptables -D INPUT -p tcp --sport 700$id -j DROP 2>/dev/null
  done
  tc qdisc del dev lo root 2>/dev/null
  scripts/cluster_local.sh stop >/dev/null 2>&1
}
trap cleanup EXIT
isolate() {
  iptables -A INPUT -p tcp --dport 700$1 -j DROP
  iptables -A INPUT -p tcp --sport 700$1 -j DROP
}
heal() { cleanup_rules; }
cleanup_rules() {
  for id in 1 2 3; do
    while iptables -D INPUT -p tcp --dport 700$id -j DROP 2>/dev/null; do :; done
    while iptables -D INPUT -p tcp --sport 700$id -j DROP 2>/dev/null; do :; done
  done
}
scripts/cluster_local.sh wipe >/dev/null 2>&1
scripts/cluster_local.sh start >/dev/null
# 2 ms +- 1 ms delay and 1% loss on loopback for the whole run (affects clients too).
tc qdisc add dev lo root netem delay 2ms 1ms loss 1%
H="$OUT/history-partition-$(date +%Y%m%d-%H%M%S).txt"
"$BIN/kvctl" --servers=$SERVERS --attempt-timeout-ms=500 --timeout-ms=5000 history \
    --out="$H" --clients=4 --seconds="$SECONDS_TOTAL" --keys=5 &
client=$!
end=$(( $(date +%s) + SECONDS_TOTAL ))
while [ "$(date +%s)" -lt "$end" ]; do
  sleep 5
  victim=$(( (RANDOM % 3) + 1 ))
  echo "isolating node $victim"; isolate $victim
  sleep $(( (RANDOM % 6) + 2 ))
  echo "healing"; heal
done
wait $client
"$BIN/lincheck" "$H"
