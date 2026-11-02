#!/bin/sh
# Reproduce K4 (run-once generated code on a DFA). Writes results/raw.csv and results/summary.txt.
# Every measured run is pinned (taskset -c $CPU); counters cover only the measured region
# (perf --control, the program enables/disables around it), user mode only.
# This VM's PMU reads 0 on the 6th programmed counter, so each config is run twice per rep:
#   group A: cycles, instructions, ic_cache_fill_l2, ic_cache_fill_sys, ls_any_fills_from_sys.dram_io_all
#   group B: cycles, ls_any_fills_from_sys.dram_io_all, l2_pf_miss_l2_l3.all
# DRAM bytes (measured, reads only) = 64 x (dram_io_all + l2_pf_miss_l2_l3.all) from group B.
set -eu
cd "$(dirname "$0")"
PERF=/usr/lib/linux-tools-6.8.0-147/perf
[ -x $PERF ] || { echo "perf missing at $PERF"; exit 1; }
CPU=${CPU:-6}
REPS=${REPS:-5}
CM=${CM:-128}
EVA=cycles:u,instructions:u,ic_cache_fill_l2:u,ic_cache_fill_sys:u,ls_any_fills_from_sys.dram_io_all:u
EVB=cycles:u,ls_any_fills_from_sys.dram_io_all:u,l2_pf_miss_l2_l3.all:u

gcc -O2 -march=native -Wall -o k4 k4.c
mkdir -p data results
sh ../e8-slices-O2/fetch-lua.sh data/lua-5.4.6 >/dev/null
sh ../common/get-chibicc.sh data/chibicc >/dev/null
ls data/lua-5.4.6/src/*.[ch] data/chibicc/*.[ch] > data/text.list
echo "text corpus: $(wc -l < data/text.list) files, $(cat $(cat data/text.list) | wc -c) bytes (repeated to 1 GiB)"
./k4 selftest seeded > results/selftest.txt
./k4 selftest text:data/text.list >> results/selftest.txt
cat results/selftest.txt
grep -q FAIL results/selftest.txt && { echo "selftest failed"; exit 1; }

D=$(mktemp -d); mkfifo $D/c $D/a
trap 'rm -rf $D' EXIT
# measure GROUP TAG ARGS... -> one csv line: tag,group,load1,<k4 fields>,<counters>
measure() {
  g=$1; tag=$2; shift 2
  [ $g = A ] && ev=$EVA || ev=$EVB
  out=$(K4_CTL=$D/c K4_ACK=$D/a $PERF stat -x, -o $D/p --control fifo:$D/c,$D/a -D -1 -e $ev -- taskset -c $CPU ./k4 "$@")
  ctrs=$(awk -F, '$3!=""{printf "%s=%s ", $3, $1}' $D/p)
  echo "$tag $g load1=$(cut -d' ' -f1 /proc/loadavg) $out $ctrs" >> results/raw.txt
}

: > results/raw.txt
echo "start $(date -u +%FT%TZ) loadavg $(cat /proc/loadavg) cpu=$CPU" | tee results/load.txt
G=${G:-1024}
for r in $(seq $REPS); do
  echo "rep $r"
  for g in A B; do
    measure $g control control seeded
    for inp in seeded text:data/text.list; do
      measure $g main table $inp 65536 0 $G
      measure $g main tableB $inp 65536 0 $G
      measure $g main interp $inp 65536 0 $G
      for ring in 4096 16384 65536; do
        measure $g main jit $inp $ring 0 $G
        measure $g main gen $inp $ring 0 $G
        measure $g main jitL $inp $ring 0 $G
        measure $g main genL $inp $ring 0 $G
      done
    done
  done
  # failure curve: 128 MiB seeded input (past the 32 MiB L3), 64 KiB ring, chunk = n input bytes
  # (3n+5 generated instructions per chunk); group A only.
  measure A curve table seeded 65536 0 $CM
  measure A curve tableB seeded 65536 0 $CM
  for n in 1 2 4 8 16 19 32 64 128 256 1024 5460; do
    measure A curve jit seeded 65536 $n $CM
    measure A curve gen seeded 65536 $n $CM
  done
done
echo "end $(date -u +%FT%TZ) loadavg $(cat /proc/loadavg)" | tee -a results/load.txt
python3 -I summarize.py results/raw.txt | tee results/summary.txt
