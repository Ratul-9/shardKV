#!/bin/bash
# Runs a 3-node lldstore cluster on this machine (Phase 8). From the project root, release build.
#   scripts/cluster_local.sh start [--sync=none]   start nodes 1-3 (data in $CLUSTER_DIR/<id>)
#   scripts/cluster_local.sh stop                   SIGTERM all nodes
#   scripts/cluster_local.sh kill <id>              kill -9 one node (a crash, not a shutdown)
#   scripts/cluster_local.sh restart <id>           start one node again on its existing data
#   scripts/cluster_local.sh status                 ask every node who leads
#   scripts/cluster_local.sh leader                 print the leader's id (empty if none)
#   scripts/cluster_local.sh wipe                   stop and delete all data
# Ports: raft 7001-7003, clients 8001-8003, metrics 9101-9103 (curl localhost:9101/metrics).
set -u
BIN=${BIN:-build/release/apps}
DIR=${CLUSTER_DIR:-/tmp/lldstore-cluster}
RAFT=1=127.0.0.1:7001,2=127.0.0.1:7002,3=127.0.0.1:7003
CLIENT=1=127.0.0.1:8001,2=127.0.0.1:8002,3=127.0.0.1:8003
SERVERS=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003
EXTRA=${EXTRA_FLAGS:-}

start_one() {
  local id=$1
  mkdir -p "$DIR/$id"
  "$BIN/kvnode" --id=$id --raft=$RAFT --client=$CLIENT --data="$DIR/$id" \
    --metrics=127.0.0.1:910$id $EXTRA >> "$DIR/$id.log" 2>&1 &
  echo $! > "$DIR/$id.pid"
}

pid_of() { cat "$DIR/$1.pid" 2>/dev/null; }

[ -x "$BIN/kvnode" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 1; }
cmd=${1:-status}
case "$cmd" in
  start)
    shift
    EXTRA="$EXTRA $*"
    mkdir -p "$DIR"
    for id in 1 2 3; do start_one $id; done
    sleep 1
    "$BIN/kvctl" --servers=$SERVERS status ;;
  stop)
    for id in 1 2 3; do p=$(pid_of $id); [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 1 ;;
  kill)
    p=$(pid_of "$2"); [ -n "$p" ] && kill -9 "$p" && echo "killed node $2 (pid $p)" ;;
  restart)
    start_one "$2"; echo "restarted node $2" ;;
  status)
    "$BIN/kvctl" --servers=$SERVERS status ;;
  leader)
    "$BIN/kvctl" --servers=$SERVERS status | sed -n 's/.*id=\([0-9]*\) role=leader.*/\1/p' | head -1 ;;
  wipe)
    "$0" stop; rm -rf "$DIR" ;;
  *)
    echo "unknown command $cmd"; exit 2 ;;
esac
