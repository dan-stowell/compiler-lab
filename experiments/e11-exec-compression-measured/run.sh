#!/bin/sh
# Reproduce E11 (measured executable compression, payload gate).
# 1. Run E9's run.sh: builds chibicc stage1/stage2 and the stage2 asm in ../e9-executable-compression/work.
# 2. Denominators, recomputed here with xz -9e and pinned by sha256:
#    xz(stage2 .o .text + .rela.text): the 9 objects' .text sections concatenated, then their
#    .rela.text sections concatenated (as E9's verify/elfsec.py does);  xz(linked stage2 .text).
# 3. e11.py: write work/payload.bin, decode it back from the file, check, report.
set -eu
cd "$(dirname "$0")"
e9=../e9-executable-compression
sh $e9/run.sh >/dev/null
rm -rf work && mkdir -p work
python3 sections.py work/stage2-objs.text+rela $e9/build/stage2/*.o
cp $e9/work/stage2.text work/stage2.text
for t in stage2-objs.text+rela stage2.text; do
  xz -9e -k -c "work/$t" > "work/$t.xz"
  echo "$t size=$(wc -c < work/$t) xz9e=$(wc -c < work/$t.xz) sha256=$(sha256sum work/$t | cut -c1-16)"
done > work/denominators.txt
# pinned inputs (E9 summary / E9 verification): fail loudly if the build drifts
grep -q '^stage2.text size=263634 xz9e=40400 sha256=e7e0c2b0ff2de72b$' work/denominators.txt || { cat work/denominators.txt; echo "linked .text denominator drifted"; exit 1; }
grep -q '^stage2-objs.text+rela size=327546 xz9e=41212 sha256=9aa5a610f39cd053$' work/denominators.txt || { cat work/denominators.txt; echo ".text+.rela.text denominator drifted"; exit 1; }
python3 e11.py $e9 work | tee summary.txt
