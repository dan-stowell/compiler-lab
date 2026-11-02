#!/bin/sh
# Reproduce K6 (run-once code for op streams executed once). Writes results/raw.txt and
# results/summary.txt. Every measured run is pinned (taskset -c $CPU, default 2); counters cover
# only the measured region (perf --control; the program enables/disables around it), user mode.
# This VM's PMU reads 0 on the 6th programmed counter, so each config runs as two groups:
#   A: cycles, instructions, branch-misses, ic_cache_fill_l2, ic_cache_fill_sys
#   B: cycles, bp_de_redirect (decoder re-steers: taken branches the BTB has never seen)
set -eu
cd "$(dirname "$0")"
PERF=/usr/lib/linux-tools-6.8.0-147/perf
[ -x $PERF ] || { echo "perf missing at $PERF"; exit 1; }
CPU=${CPU:-2}   # vCPUs pair up on L1/L2 (0-1, 2-3, 4-5, 6-7): pick a pair with no other job
REPS=${REPS:-5}          # repetitions for the large streams
NREPS=${NREPS:-11}       # the natural stream is ~16K records, so more repetitions
QS=${QS:-"0 250 500 700 800 900 950 980 1000"}
EVA=cycles:u,instructions:u,branch-misses:u,ic_cache_fill_l2:u,ic_cache_fill_sys:u
EVB=cycles:u,bp_de_redirect:u

mkdir -p data results
sh ../e8-slices-O2/fetch-lua.sh data/lua-5.4.6 >/dev/null
gcc -O2 -march=native -Wall -o k6syn k6syn.c -lm
gcc -O2 -march=native -Wall -Wno-misleading-indentation -Idata/lua-5.4.6/src -o k6lua k6lua.c -lm
[ -x data/luatr/lua-trace ] || tracer/build.sh data/lua-5.4.6 data/luatr
# natural stream: main chunks of real Lua modules (Neovim runtime + LuaJIT jit/*), once each
(find /usr/share/nvim/runtime -name '*.lua'; find /usr/share/luajit-2.1 -name '*.lua') | sort > data/natural.list
K6_TRACE=data/natural.tr data/luatr/lua-trace lua/natural.lua data/natural.list > results/natural-run.txt
K6_TRACE=data/bench.tr data/luatr/lua-trace lua/bench.lua lua > results/bench-run.txt
{ ./k6lua convert data/natural.tr data/natural-once.k6p once
  ./k6lua convert data/natural.tr data/natural-full.k6p full
  ./k6lua convert data/bench.tr data/bench-full.k6p full; } > results/convert.txt
cat results/convert.txt
# correctness: every implementation reproduces the converter's final state (all registers and acc)
: > results/check.txt
for q in $QS; do ./k6syn check $q 22 >> results/check.txt; ./k6syn stats $q >> results/check.txt; done
for p in natural-once natural-full bench-full; do ./k6lua check data/$p.k6p >> results/check.txt; ./k6lua stats data/$p.k6p >> results/check.txt; done
cat results/check.txt
grep -q FAIL results/check.txt && { echo "check failed"; exit 1; }

D=$(mktemp -d); mkfifo $D/c $D/a
trap 'rm -rf $D' EXIT
measure() {  # measure GROUP TAG PROG ARGS...
  g=$1; tag=$2; shift 2
  [ $g = A ] && ev=$EVA || ev=$EVB
  out=$(K6_CTL=$D/c K6_ACK=$D/a $PERF stat -x, -o $D/p --control fifo:$D/c,$D/a -D -1 -e $ev -- taskset -c $CPU "$@")
  ctrs=$(awk -F, '$3!=""{printf "%s=%s ", $3, $1}' $D/p)
  echo "$tag $g load1=$(cut -d' ' -f1 /proc/loadavg) $out | $ctrs" >> results/raw.txt
}
: > results/raw.txt
echo "start $(date -u +%FT%TZ) loadavg $(cat /proc/loadavg) cpu=$CPU" | tee results/load.txt
for r in $(seq $REPS); do
  echo "rep $r"
  for g in A B; do
    for q in $QS; do for m in cgoto pred hybrid jit gen; do measure $g syn ./k6syn $m $q; done; done
    for p in natural-full bench-full; do for m in cgoto hybrid jit gen; do measure $g lua ./k6lua $m data/$p.k6p; done; done
  done
done
for r in $(seq $NREPS); do
  for g in A B; do for m in cgoto hybrid jit gen; do measure $g lua ./k6lua $m data/natural-once.k6p; done; done
done
echo "end $(date -u +%FT%TZ) loadavg $(cat /proc/loadavg)" | tee -a results/load.txt
python3 -I summarize.py results/raw.txt | tee results/summary.txt
