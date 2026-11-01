#!/bin/sh
# Reproduce E5 from scratch: fetch+build chibicc, preprocess parse.c, compute and
# verify every function's closure slice (strict prototypes, and callee stubs).
set -eu
cd "$(dirname "$0")"
sh ../common/get-chibicc.sh chibicc
make -s -C chibicc chibicc
(cd chibicc && ./chibicc -E parse.c -o ../parse.i)
python3 slice.py chibicc/chibicc parse.i slices-strict.csv
python3 slice.py chibicc/chibicc parse.i slices-stubs.csv --stubs
python3 summarize.py parse.i slices-strict.csv slices-stubs.csv | tee summary.txt
