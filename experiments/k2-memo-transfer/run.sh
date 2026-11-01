#!/bin/sh
# K2: memo-table transfer in 1-D HashLife (journal/phase2-plan.md, K2). ~8 min, ~3 GB RAM.
set -eu
cd "$(dirname "$0")"
gcc -O2 -Wall -Wextra -o k2 k2.c
mkdir -p results
{
  # correctness: HashLife vs direct bit-sliced stepping (first line of each = pure background)
  ./k2 validate 110 12 256 1 2 3 4 5
  ./k2 validate 30 12 256 1 2 3 4 5
  ./k2 validate 110 16 1024 11 12
  ./k2 validate 30 16 256 11
} | tee results/validate.txt
./k2 transfer 110 24 256 20 1000 | tee results/r110-T24-P256.txt
for P in 64 1024 4096; do ./k2 transfer 110 24 $P 20 1000 > results/r110-T24-P$P.txt; done
for T in 16 20; do ./k2 transfer 110 $T 256 20 1000 > results/r110-T$T-P256.txt; done
./k2 transfer 30 24 256 20 1000 | tee results/r30-T24-P256.txt
./k2 transfer 30 15 256 20 1000 | tee results/r30-T15-P256.txt
grep -h -E '^(# |frozen|summary|rule|A:)' results/r*.txt | tee results/summary.txt
