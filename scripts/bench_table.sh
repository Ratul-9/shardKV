#!/bin/bash
set -u
REPS=${1:-5}
OUT=${RESULTS_DIR:-bench/results}
mkdir -p "$OUT"
for b in table_bench footprint; do
  [ -x build/release/bench/$b ] || { echo "build first: cmake --build --preset release"; exit 1; }
done
header() { { echo "# commit=$(git rev-parse --short HEAD) date=$(date +%F_%T) reps=$REPS"; uptime; } > "$1"; }

F=$OUT/phase5_table_bench.txt; header $F
build/release/bench/table_bench --benchmark_repetitions="$REPS" --benchmark_report_aggregates_only=true \
  --benchmark_min_time=0.3s >> $F 2>&1
uptime >> $F

F=$OUT/phase5_footprint.txt; header $F
for n in 100000 1000000; do
  for t in map flat; do build/release/bench/footprint $n $t >> $F; done
done
echo done
