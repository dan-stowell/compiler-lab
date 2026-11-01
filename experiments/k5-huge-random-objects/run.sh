#!/bin/sh
# K5: huge random objects as split trees (journal/phase2-plan.md, K5). ~10 min on 8 cores.
set -eu
cd "$(dirname "$0")"
mkdir -p results
gcc -O2 -march=native -Wall -Wextra -o results/k5 k5.c -lm
B=results/k5
$B binomtest | tee results/binomtest.txt
# Correctness: materialize + sort + merge vs tree operations (sparse, dense, dense-overlapping, odd sizes).
{ for N in 1048576 67108864; do
    $B verify $N 0; $B verify $N $((4*N)); $B verify $N $((4*N)) $((3*N/2)); $B verify $((N+3)) $((4*N-1)) 12345
  done; } | tee results/verify.txt
# (a) range count visits, N = 2^16..2^40 (range sum visits up to 2^30).
for e in $(seq 16 2 40); do N=$((1<<e)); $B range $N $((4*N)); done | tee results/range.txt
# (b) join visits. W = 4N (dense), 1024N (sparse-ish), 2^64 (almost empty join).
# Measured: full descent, N = 2^16..2^30. Estimated: exact top 2^k pairs + 64 sampled subtrees of ~2^22, N = 2^30..2^40.
{ for e in $(seq 16 2 30); do N=$((1<<e)); for W in $((4*N)) $((1024*N)) 0; do echo "$B join $N $W"; done; done
  for e in $(seq 30 2 40); do N=$((1<<e)); for W in $((4*N)) $((1024*N)) 0; do echo "$B est $N $W $((e-22)) 64"; done; done
} | xargs -P "${JOBS:-$(nproc)}" -I{} sh -c '{}' | sort -k2,2n -k1,1 > results/join.txt
cat results/join.txt
python3 -I fit.py results/join.txt | tee results/fit.txt
