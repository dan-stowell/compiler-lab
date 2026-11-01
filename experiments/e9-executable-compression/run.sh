#!/bin/sh
# Reproduce E9 step 1 (the decider) from scratch.
# 1. Build chibicc 90d1f7f with gcc (stage1), then stage2 = stage1 compiling chibicc.
# 2. Emit stage2's asm (stage1 -S, checked against stage2 -S and against stage2's .o .text).
# 3. Denominator: xz -9e of stage2's linked .text. Context: clang -Oz build, same measure.
# 4. e9.py: parse asm, Re-Pair (two variants), lower bound, ratio.
set -eu
cd "$(dirname "$0")"
here=$(pwd)
sh ../common/get-chibicc.sh chibicc
rm -rf build build-oz work && mkdir -p build build-oz work/s2 work/s3
git -C chibicc archive 90d1f7f | tar -x -C build
git -C chibicc archive 90d1f7f | tar -x -C build-oz
make -C build -j8 chibicc >/dev/null 2>&1
make -C build -j8 stage2/chibicc >/dev/null 2>&1
ln -s ../include build/stage2/include  # stage2 finds headers next to itself
CFLAGS="-std=c11 -g -fno-common -Wall -Wno-switch"
srcs=$(cd build && ls *.c)
for f in $srcs; do
  b=${f%.c}
  (cd build && ./chibicc $CFLAGS -S -o "$here/work/s2/$b.s" "$f")
  (cd build && ./stage2/chibicc $CFLAGS -S -o "$here/work/s3/$b.s" "$f")
  # only the .file paths of the bundled headers may differ (./include vs ./stage2/include)
  grep -v '^  \.file ' "work/s2/$b.s" > work/s2.tmp; grep -v '^  \.file ' "work/s3/$b.s" > work/s3.tmp
  cmp -s work/s2.tmp work/s3.tmp || { echo "stage2 -S differs from stage1 -S for $f"; exit 1; }
  as -o "work/s2/$b.o" "work/s2/$b.s"
  objcopy -O binary --only-section=.text "work/s2/$b.o" "work/s2/$b.as.text"
  objcopy -O binary --only-section=.text "build/stage2/$b.o" "work/s2/$b.o.text"
  cmp -s "work/s2/$b.as.text" "work/s2/$b.o.text" || { echo ".text of as(asm) != stage2 $b.o"; exit 1; }
done
echo "asm check: stage1 -S == stage2 -S (except .file lines), and as(asm) .text == stage2 .o .text, for all $(echo $srcs | wc -w) TUs"
objcopy -O binary --only-section=.text build/stage2/chibicc work/stage2.text
for f in $srcs; do cat "work/s2/${f%.c}.o.text"; done > work/stage2-objs.text
make -C build-oz -j8 chibicc CC=clang CFLAGS="-std=c11 -Oz -fno-common -Wno-switch" >/dev/null 2>&1
objcopy -O binary --only-section=.text build-oz/chibicc work/clang-oz.text
for t in stage2 stage2-objs clang-oz; do
  xz -9e -k -c "work/$t.text" > "work/$t.text.xz"
  echo "$t.text size=$(wc -c < work/$t.text) xz9e=$(wc -c < work/$t.text.xz) sha256=$(sha256sum work/$t.text | cut -c1-16)"
done | tee work/denominators.txt
python3 e9.py work/s2 work/denominators.txt | tee summary.txt
