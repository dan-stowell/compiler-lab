#!/bin/sh
# E14: lifetime of TU-free function keys (journal/cycle6-plan.md, E14).
# Reuses E10's preprocessed history (experiments/e10-hidden-deps/work: tok.tsv + cas/);
# runs E10's run.sh first if that data is absent (~30 min).
set -eu
cd "$(dirname "$0")"
JOBS=${JOBS:-$(nproc)}
E10W=../e10-hidden-deps/work
[ -f "$E10W/tok.tsv" ] || sh ../e10-hidden-deps/run.sh
mkdir -p results
python3 -I e14.py keys "$E10W" results/keys.json "$JOBS"
python3 -I e14.py analyze "$E10W" results/keys.json | tee results/summary.txt
