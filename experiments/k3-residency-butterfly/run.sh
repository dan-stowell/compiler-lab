#!/bin/sh
# Reproduce K3 (residency on a butterfly DAG). Writes summary.txt and perf.csv.
# DRAM bytes (measured) = 64 x (ls_any_fills_from_sys.dram_io_all + l2_pf_miss_l2_l3.all),
# user-mode only (:u), median of 5 runs. These events count fills (reads) only.
set -eu
cd "$(dirname "$0")"
PERF=/usr/lib/linux-tools-6.8.0-147/perf
[ -x $PERF ] || { echo "perf missing at $PERF"; exit 1; }
gcc -O3 -march=native -Wall -Wextra -Werror -o k3 k3.c
EV=ls_any_fills_from_sys.dram_io_all:u,l2_pf_miss_l2_l3.all:u,instructions:u,cycles:u
K=${K:-9}        # model block level (9 minimises butterflies, see sweep)
CTRL=${CTRL:-5000} # resident-control repetitions (roughly cycle-matched to the model)

# one measurement: prints "dram_bytes dram_fills pf_miss instructions cycles" (median of 5 by DRAM bytes)
meas() {
    for rep in 1 2 3 4 5; do
        $PERF stat -x, -e $EV -o perf.tmp "$@" > /dev/null
        awk -F, '/dram_io_all/{d=$1} /l2_pf_miss/{p=$1} /instructions/{i=$1} /cycles/{c=$1}
                 END{printf "%d %d %d %d %d\n", 64*(d+p), d, p, i, c}' perf.tmp
    done | sort -n | sed -n 2p
    rm -f perf.tmp
}

{
echo "machine: $(lscpu | sed -n 's/^Model name: *//p'), L2 1 MiB/core, L3 32 MiB, $(uname -r), $(gcc --version | head -1)"
echo
echo "== correctness: all schedules give the same hash =="
for lg in 10 12 14 16 18 20 22 24; do
    hs=$(for m in mat blocked blocked-rs model; do ./k3 $m $lg $K | sed 's/.*hash=\([0-9a-f]*\).*/\1/'; done | sort -u)
    [ "$(echo "$hs" | wc -l)" -eq 1 ] || { echo "log2n=$lg: MISMATCH $hs"; exit 1; }
    echo "log2n=$lg hash=$hs OK"
done
echo
echo "== calibration (1 GiB = 1073741824 B) =="
set -- $(meas ./k3 calib-read touch-only); t=$1
set -- $(meas ./k3 calib-read); r=$1
set -- $(meas ./k3 calib-ntw); w=$1
echo "touch-only (memset 1 GiB): $t B"
echo "touch + 1 GiB read sweep:  $r B  -> read sweep alone = $((r - t)) B = $(awk "BEGIN{printf \"%.3f\", ($r-$t)/1073741824}") GiB"
echo "1 GiB non-temporal write:  $w B  (writes are not counted by these fill events)"
echo
echo "mode,log2n,dram_bytes_measured,dram_fills,l2_pf_miss,instructions,cycles,butterflies,leaves,recompute_factor,computed_read,computed_write" > perf.csv
for lg in 16 24; do
    for m in mat blocked blocked-rs model; do
        line=$(./k3 $m $lg $K)
        get() { echo "$line" | sed "s/.*$1=\([0-9.]*\).*/\1/"; }
        set -- $(meas ./k3 $m $lg $K)
        echo "$m,$lg,$1,$2,$3,$4,$5,$(get butterflies),$(get leaves),$(get recompute_factor),$(get computed_dram_read),$(get computed_dram_write)" >> perf.csv
    done
done
set -- $(meas ./k3 resident 0 $CTRL)
echo "resident-control,24,$1,$2,$3,$4,$5,$(./k3 resident 0 $CTRL | sed 's/.*butterflies=//'),0,,0,0" >> perf.csv
echo "== model block-level sweep, log2n=24 (counted butterflies; pick k) =="
for k in 7 8 9 10 11 12 13 14; do ./k3 model 24 $k | awk '{print $3, $5, $6, $7}'; done
echo
echo "== perf.csv =="
column -s, -t perf.csv
echo
awk -F, '$2!=24 {next} $1=="blocked" {b=$3; bi=$6} $1=="blocked-rs" {brs=$3} $1=="model" {m=$3; mi=$6; mc=$7}
         $1=="resident-control" {c=$3; cc=$7}
         END{best=(b<brs)?b:brs; floor=c/cc*mc;
             printf "measured DRAM bytes per Gcycle: model %.0f, resident control %.0f\n", m/mc*1e9, c/cc*1e9;
             printf "control floor scaled to model cycles (estimate): %.0f B; model minus floor: %.0f B\n", floor, m-floor;
             printf "rule (log2n=24), measured: best blocked %d B / model %d B = %.1fx (need >=100x)\n", best, m, best/m;
             printf "rule (log2n=24), computed: blocked 134217728 B spill traffic / model 0 B\n";
             printf "model/blocked instructions = %.1fx, cycles = %.1fx\n", mi/bi, mc/cyc}' cyc=$(awk -F, '$1=="blocked" && $2==24 {print $7}' perf.csv) perf.csv
} | tee summary.txt
