#!/bin/sh
# K1: grammar-resident data vs a chunk-summary store (journal/phase2-plan.md, K1).
#   1. synthetic: 64 MiB base, 10^4 versions of 1..10 small overwrites each
#   2. real: every committed version of lua/lua lvm.c (795 commits; small)
# Output: results/synth.txt, results/lua.txt
set -eu
cd "$(dirname "$0")"
N=${N:-67108864}
V=${V:-10000}
gcc --version | head -1
gcc -O2 -march=native -Wall -Wextra -o k1 k1.c
mkdir -p data results
[ -d data/lua ] || git clone -q https://github.com/lua/lua data/lua
python3 -I extract.py data/lua data/lvm.versions
./k1 files data/lvm.versions 32,64,256 64,256,1024,4096 | tee results/lua.txt
/usr/bin/time -f "max RSS %M KiB, %e s" ./k1 synth "$N" "$V" 1 16,64,256 256,1024,4096,16384,65536 2>&1 | tee results/synth.txt
