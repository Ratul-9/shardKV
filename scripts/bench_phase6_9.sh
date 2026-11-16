#!/bin/bash
# Phases 6, 7 and 9 measurements (in process). Linux machine, release build, project root.
#   scripts/bench_phase6_9.sh [reps=3]
# Environment: RESULTS_DIR (default bench/results/linux), PIN_CPUS (as bench_sharded.sh),
#              WAL_DIR (a directory ON THE DISK TO MEASURE; default /tmp, which may be tmpfs!)
set -u
REPS=${1:-3}
OUT=${RESULTS_DIR:-bench/results/linux}
PIN_CPUS=${PIN_CPUS-0,1,2,3,4,5}
WAL_DIR=${WAL_DIR:-/tmp/lldstore-walbench}
B=build/release/bench
mkdir -p "$OUT"
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS pin_cpus=$PIN_CPUS"; uptime; } > "$1"; }
PIN=""; [ -n "$PIN_CPUS" ] && PIN="--pin=$PIN_CPUS"

# Phase 6: footprint (requested bytes and RSS) for the three tables, 1M keys.
F=$OUT/phase6_footprint.txt; header $F
for t in map flat slab; do echo "## table=$t" >> $F; $B/footprint 1000000 $t >> $F; done

# Phase 6: request path with and without per-request allocation (same queue design otherwise
# differs: lockfree = node MPSC + FlatStore heap entries; slab = intrusive MPSC + slab entries).
F=$OUT/phase6_closed.txt; header $F
for rep in $(seq 1 "$REPS"); do
  for store in lockfree slab; do
    for c in 1 4 12 64; do
      echo "## store=$store table=flat shards=6 clients=$c rep=$rep" >> $F
      if [ $store = lockfree ]; then T=--table=flat; else T=; fi
      $B/contention --store=$store $T --shards=6 $PIN --clients=$c --mode=closed --keys=1000000 --seconds=4 --warmup=1 >> $F
    done
  done
done
F=$OUT/phase6_open.txt; header $F
for rep in $(seq 1 "$REPS"); do
  for store in lockfree slab; do
    for rate in 200000 1000000; do
      echo "## store=$store shards=6 clients=12 rate=$rate rep=$rep" >> $F
      if [ $store = lockfree ]; then T=--table=flat; else T=; fi
      $B/contention --store=$store $T --shards=6 $PIN --clients=12 --mode=open --rate=$rate --keys=1000000 --seconds=6 --warmup=1 >> $F
    done
  done
done

# Phase 7: group commit on the real disk. Record what the disk is: findmnt -T $WAL_DIR.
F=$OUT/phase7_wal.txt; header $F
findmnt -T "$WAL_DIR" >> $F 2>/dev/null || df "$WAL_DIR" >> $F
for rep in $(seq 1 "$REPS"); do
  echo "## sync=data delay_us=0 rep=$rep" >> $F; $B/wal_bench --dir="$WAL_DIR" --sync=data >> $F
  echo "## sync=data delay_us=200 rep=$rep" >> $F; $B/wal_bench --dir="$WAL_DIR" --sync=data --delay-us=200 >> $F
  echo "## sync=none rep=$rep" >> $F; $B/wal_bench --dir="$WAL_DIR" --sync=none --writers=1,8,32 >> $F
done

# Phase 9: readers with one writer: RCU vs shared_mutex vs mutex.
F=$OUT/phase9_rcu.txt; header $F
for rep in $(seq 1 "$REPS"); do
  for map in rcu shared mutex; do
    for r in 1 2 4 8; do
      echo "## map=$map readers=$r writer=1 rep=$rep" >> $F
      $B/rcu_bench --map=$map --readers=$r --seconds=3 >> $F
    done
  done
done
echo "results in $OUT"
