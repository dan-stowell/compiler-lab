#!/bin/sh
# Reproduce E7 from scratch: fetch chibicc, build HEAD 90d1f7f once, compile every
# historical (first-parent, non-test) .c file with it, hash per-function asm, summarize.
# Compiles are cached in work/asm by key (file + headers), so reruns only do new work.
set -eu
cd "$(dirname "$0")"
here=$(pwd)
sh ../common/get-chibicc.sh chibicc
if [ ! -x build/chibicc ]; then
  rm -rf build && mkdir build
  git -C chibicc archive 90d1f7f | tar -x -C build
  make -C build -j8 chibicc >/dev/null
fi
python3 e7.py plan chibicc 90d1f7f "$here/work"
start=$(date +%s)
xargs -P8 -L1 sh "$here/compile.sh" "$here/build/chibicc" "$here/work" < work/jobs.txt
echo "compile wall: $(( $(date +%s) - start ))s for $(wc -l < work/jobs.txt) jobs"
python3 e7.py analyze chibicc 90d1f7f "$here/work" ripple.csv
python3 summarize.py ripple.csv work | tee summary.txt
