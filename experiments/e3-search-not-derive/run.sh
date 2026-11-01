#!/bin/sh
# E3: build, run the search on all 200 expressions, summarise. Needs gcc and python3.
# Usage: ./run.sh [JOBS]   (default: nproc; CPU time is measured per process so parallelism doesn't change it)
set -eu
cd "$(dirname "$0")"
jobs=${1:-$(nproc)}
gcc -O3 -march=native -Wall -Wextra -o e3 e3.c
./e3 list > expressions.tsv
seq 0 199 | xargs -P "$jobs" -I{} ./e3 solve {} 600 > results.unsorted.tsv
sort -n results.unsorted.tsv > results.tsv && rm results.unsorted.tsv
python3 summarize.py results.tsv | tee summary.md
