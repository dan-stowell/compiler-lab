#!/bin/sh
# E16: layout-projection key vs E15's whole-unit output (journal/cycle7-plan.md).
# Needs E15's results (runs E15's run.sh if absent). About 5 min on 8 cores.
set -eu
cd "$(dirname "$0")"
JOBS=${JOBS:-$(nproc)}
E10W=../e10-hidden-deps/work
E15R=../e15-output-churn/results
[ -f "$E15R/outputs.json" ] || sh ../e15-output-churn/run.sh
mkdir -p results
python3 -I e16.py mutate "$E15R" | tee results/mutate.txt
grep -q "all pass" results/mutate.txt   # dash has no pipefail
# cost pilot: first 300 slice-changed transitions (seeded order), then all
python3 -I e16.py keys "$E10W" "$E15R" results 300 "$JOBS" | tee results/keys300.log
python3 -I e16.py analyze "$E10W" "$E15R" results 300 | tee results/summary300.txt
python3 -I e16.py keys "$E10W" "$E15R" results all "$JOBS" | tee results/keys.log
python3 -I e16.py analyze "$E10W" "$E15R" results all | tee results/summary.txt
python3 -I e16.py explain "$E10W" "$E15R" results | tee results/explain.txt
