#!/bin/bash
# Phase 2 gRPC end-to-end matrix. Server and client run on the same machine (they compete for cores).
#   scripts/bench_grpc.sh [reps=3] [mode=all|closed|open]
# Writes bench/results/phase2_grpc_closed.txt and phase2_grpc_open.txt. Run from the project root.
set -u
SERVER=build/release/bench/kv_server
CLIENT=build/release/bench/kv_client
REPS=${1:-3}
MODE=${2:-all}
PORT=50321
OUT=${RESULTS_DIR:-bench/results}   # set RESULTS_DIR=bench/results/linux to keep other machines apart
mkdir -p "$OUT"
[ -x "$SERVER" ] && [ -x "$CLIENT" ] || { echo "build first: cmake --build --preset release"; exit 1; }
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS"; uptime; } > "$1"; }
CF=$OUT/phase2_grpc_closed.txt; OF=$OUT/phase2_grpc_open.txt
DO_CLOSED=0; DO_OPEN=0
{ [ "$MODE" = all ] || [ "$MODE" = closed ]; } && DO_CLOSED=1
{ [ "$MODE" = all ] || [ "$MODE" = open ]; } && DO_OPEN=1
[ $DO_CLOSED = 1 ] && header $CF
[ $DO_OPEN = 1 ] && header $OF

# config = "store:workers"   workers=0 means handlers run inline on gRPC's threads
for config in mutex:0 mutex:8 shared:8; do
  store=${config%%:*}; workers=${config##*:}
  rm -f /tmp/kv_server_bench.log
  $SERVER --store=$store --workers=$workers --keys=1000000 --port=$PORT > /tmp/kv_server_bench.log 2>&1 &
  SPID=$!
  ready=0
  for i in $(seq 1 300); do
    grep -q READY /tmp/kv_server_bench.log 2>/dev/null && { ready=1; break; }
    sleep 0.1
  done
  [ $ready = 1 ] || { echo "server $config did not become ready"; kill $SPID 2>/dev/null; exit 1; }
  for c in 1 4 16 64; do
    if [ $DO_CLOSED = 1 ]; then
      for rep in $(seq 1 "$REPS"); do
        echo "## server=$store workers=$workers clients=$c mode=closed rep=$rep" >> $CF
        $CLIENT --target=127.0.0.1:$PORT --clients=$c --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $CF
      done
    fi
    if [ $DO_OPEN = 1 ]; then
      echo "## server=$store workers=$workers clients=$c mode=open rate=5000" >> $OF
      $CLIENT --target=127.0.0.1:$PORT --clients=$c --mode=open --rate=5000 --keys=1000000 --seconds=6 --warmup=1 >> $OF
    fi
  done
  kill -TERM $SPID; wait $SPID 2>/dev/null
done
[ $DO_CLOSED = 1 ] && uptime >> $CF
[ $DO_OPEN = 1 ] && uptime >> $OF
echo done
