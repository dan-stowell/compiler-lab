#!/bin/sh
# E15: whole-unit output churn of E13 units over lua/lua history (journal/cycle7-plan.md).
# Reuses E10's preprocessed history (../e10-hidden-deps/work: tok.tsv + cas/);
# runs E10's run.sh first if that data is absent (~30 min). About 5 min on 8 cores.
set -eu
cd "$(dirname "$0")"
JOBS=${JOBS:-$(nproc)}
E10W=../e10-hidden-deps/work
[ -f "$E10W/tok.tsv" ] || sh ../e10-hidden-deps/run.sh
mkdir -p results
clang-18 --version | head -1 | tee results/cc.txt
python3 -I e15.py units "$E10W" results "$JOBS"
python3 -I e15.py compile "$E10W" results "$JOBS" | tee results/compile.log
python3 -I e15.py analyze "$E10W" results | tee results/summary.txt
python3 -I e15.py check "$E10W" results 300 | tee results/check.txt
python3 -I e15.py attr "$E10W" results | tee results/attr.txt
