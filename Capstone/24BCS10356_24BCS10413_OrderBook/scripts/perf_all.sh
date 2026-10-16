#!/usr/bin/env bash
# Collect perf stat + perf record/report for all three implementations (Linux only).
# Usage: ./scripts/perf_all.sh            (from the project root, after `make`)
#
# Uses `./bench --impl X --perf`: one uninstrumented pass, so the counters
# measure the order book rather than the per-call timers. The exact command
# from the assignment (`./bench --impl X`, 5 timed runs) also works with perf.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p results
EVENTS=task-clock,cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults

for IMPL in A B C; do
  echo "=== perf stat: impl $IMPL ==="
  perf stat -r 5 -e "$EVENTS" ./bench --impl "$IMPL" --perf 2>&1 | tee "results/perf_stat_$IMPL.txt"

  echo "=== perf record: impl $IMPL ==="
  perf record -g -o "results/perf_$IMPL.data" ./bench --impl "$IMPL" --perf >/dev/null 2>&1
  # Write the full report first, then keep the top part. Piping straight into
  # `head` would kill perf with SIGPIPE and, with pipefail, stop the script.
  perf report -i "results/perf_$IMPL.data" --stdio --no-children > "results/perf_report_${IMPL}_full.txt" 2>/dev/null
  sed -n '1,120p' "results/perf_report_${IMPL}_full.txt" > "results/perf_report_$IMPL.txt"
  rm -f "results/perf_report_${IMPL}_full.txt"
  echo "wrote results/perf_report_$IMPL.txt"
done
