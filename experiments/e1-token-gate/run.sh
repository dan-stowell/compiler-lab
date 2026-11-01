#!/bin/sh
# E1 token gate: reproduce everything from scratch.
#   1. fetch pinned chibicc (pristine) and build it
#   2. make a patched copy (chibicc-memstat.patch + memstat.c) and build it
#   3. sanity: both compilers must emit identical parse.s for pristine parse.c
#   4. run the instrumented cc1 once for byte accounting
#   5. time / peak RSS: 5 runs each, median (pristine and patched)
#   6. write RESULTS.md
# Input is always the pristine chibicc/parse.c (the patch touches chibicc.h,
# which parse.c includes, so the patched tree's parse.c is a different input).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
out=$here/out
rm -rf "$out" chibicc-memstat
mkdir -p "$out"

../common/get-chibicc.sh chibicc
make -s -C chibicc clean
make -s -C chibicc chibicc

cp -r chibicc chibicc-memstat
rm -rf chibicc-memstat/.git
make -s -C chibicc-memstat clean
patch -s -d chibicc-memstat -p1 < chibicc-memstat.patch
cp memstat.c chibicc-memstat/
make -s -C chibicc-memstat chibicc
# chibicc derives its include dir from argv[0], and the path lands in `.file`
# directives; run the instrumented binary from the pristine dir so both
# compilers see identical include paths.
cp chibicc-memstat/chibicc chibicc/chibicc-memstat

input=$here/chibicc/parse.c
base=$here/chibicc/chibicc
inst=$here/chibicc/chibicc-memstat

# Invoke cc1 directly (what the driver forks for `chibicc -S parse.c`).
cc1() { "$1" -cc1 -cc1-input "$input" -cc1-output "$2" "$input"; }

# Sanity: driver output == direct cc1 output == instrumented cc1 output.
"$base" -S -o "$out/parse.driver.s" "$input"
cc1 "$base" "$out/parse.base.s"
MEMSTAT_OUT=$out/memstat.txt cc1 "$inst" "$out/parse.inst.s"
cmp "$out/parse.driver.s" "$out/parse.base.s"
cmp "$out/parse.base.s" "$out/parse.inst.s"
echo "sanity: parse.s identical (driver, cc1, instrumented cc1)"

# Timing and peak RSS, 5 runs each.
for which in base inst; do
  eval bin=\$$which
  for i in 1 2 3 4 5; do
    MEMSTAT_OUT=/dev/null /usr/bin/time -v -o "$out/time.$which.$i" \
      "$bin" -cc1 -cc1-input "$input" -cc1-output "$out/t.s" "$input"
  done
done
rm -f "$out/t.s"

python3 -I "$here/report.py" "$out" > RESULTS.md
cat RESULTS.md
