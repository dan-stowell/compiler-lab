#!/bin/sh
# Reproduce E12 (hot/cold frontier for executable compression) from scratch.
# 1. E11's run.sh: builds chibicc 90d1f7f stage1/stage2 (via E9's run.sh) and E11's payload.bin.
# 2. C decoder (dec.c, -O2): round trip vs E11's Python decoder (text dumps, cmp), .text size,
#    timing of sequential whole-payload decode (median of 1001 repetitions).
# 3. Profile: native stage2 `-cc1` compiling tokenize.c under callgrind (--dump-instr=yes);
#    per-function self Ir by address, native sizes from nm address deltas (profile.py).
# 4. Native wall time (median of 5 after a warm-up), then the frontier and decision (frontier.py).
set -eu
cd "$(dirname "$0")"
e9=../e9-executable-compression
e11=../e11-exec-compression-measured
command -v valgrind >/dev/null || { echo "valgrind missing: sudo apt-get install -y valgrind"; exit 1; }
sh $e11/run.sh >/dev/null
rm -rf work && mkdir -p work
payload=$e11/work/payload.bin
build=$(cd $e9/build && pwd)
out=$(pwd)/work

{
echo "machine: $(lscpu | sed -n 's/^Model name: *//p'), $(nproc) vCPUs, $(uname -r), $(gcc --version | head -1)"

# ---- 2. C decoder
gcc -O2 -Wall -Wextra -Werror -c dec.c -o work/dec.o
gcc -O2 -Wall -Wextra -Werror e12dec.c work/dec.o -o work/e12dec
dtext=$(size -A work/dec.o | awk '$1==".text" {print $2}')
python3 dump_e11.py $e11 $e9 work/e11rebuild work/e11-python.txt
work/e12dec $payload dump > work/e12-c.txt
cmp work/e11-python.txt work/e12-c.txt || { echo "C decoder round trip: FAIL"; exit 1; }
echo "C decoder round trip vs E11 Python decoder: OK ($(wc -l < work/e12-c.txt) instructions, identical start/skeleton/values)"
echo "C decoder .text (dec.o: init + decode) = $dtext B"
work/e12dec $payload time 1001 | tee work/dectime.txt
nsi=$(sed 's/.*median_ns_per_instr=\([0-9.]*\).*/\1/' work/dectime.txt)
tab=$(sed 's/.*table_bytes=\([0-9]*\).*/\1/' work/dectime.txt)

# ---- 3. profile
(cd $build && valgrind --tool=callgrind --dump-instr=yes --callgrind-out-file=$out/cg.out \
   ./stage2/chibicc -cc1 -cc1-input tokenize.c -cc1-output $out/t-cg.s tokenize.c 2>/dev/null)
(cd $build && ./stage2/chibicc -S -o $out/t-S.s tokenize.c)
cmp work/t-cg.s work/t-S.s && echo "-cc1 output under callgrind == chibicc -S tokenize.c"
python3 profile.py work/cg.out $build/stage2/chibicc $e9/work/s2 profile.csv
total=$(sed -n 's/^summary: //p' work/cg.out)

# ---- 4. native time, frontier
python3 walltime.py 5 $build ./stage2/chibicc -cc1 -cc1-input tokenize.c -cc1-output $out/t.s tokenize.c | tee work/wall.txt
T=$(sed 's/native_wall_median_s=\([0-9.]*\).*/\1/' work/wall.txt)
text=$(readelf -SW $build/stage2/chibicc | awk '$2==".text" {print strtonum("0x" $6)}')
python3 frontier.py profile.csv $total $T $nsi $tab $dtext $(wc -c < $payload) $text 5292 frontier.csv
} | tee summary.txt
