#!/bin/bash
# Phase 2 gRPC end-to-end matrix. Server and client run on the same machine (they compete for cores).
#   scripts/bench_grpc.sh [reps=3]
# Writes bench/results/phase2_grpc.txt. Run from the project root, release build.
set -u
SERVER=build/release/bench/kv_server
CLIENT=build/release/bench/kv_client
REPS=${1:-3}
PORT=50321
OUT=bench/results/phase2_grpc.txt
[ -x "$SERVER" ] && [ -x "$CLIENT" ] || { echo "build first: cmake --build --preset release"; exit 1; }
{ echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS"; uptime; } > $OUT

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
    for rep in $(seq 1 "$REPS"); do
      echo "## server=$store workers=$workers clients=$c mode=closed rep=$rep" >> $OUT
      $CLIENT --target=127.0.0.1:$PORT --clients=$c --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $OUT
    done
    echo "## server=$store workers=$workers clients=$c mode=open rate=5000" >> $OUT
    $CLIENT --target=127.0.0.1:$PORT --clients=$c --mode=open --rate=5000 --keys=1000000 --seconds=6 --warmup=1 >> $OUT
  done
  kill -TERM $SPID; wait $SPID 2>/dev/null
done
uptime >> $OUT
echo done
