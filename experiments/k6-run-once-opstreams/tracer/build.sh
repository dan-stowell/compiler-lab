#!/bin/sh
# build.sh LUA_SRC_DIR OUT_DIR: builds OUT_DIR/lua (stock) and OUT_DIR/lua-trace (tracer in vmfetch)
set -eu
src=$1; out=$2; here=$(cd "$(dirname "$0")" && pwd)
rm -rf "$out"; mkdir -p "$out"; cp -r "$src"/src "$out"/src
cp "$here"/k6trace.h "$out"/src/
# insert the tracer after the fetch: i = *(pc++);  (pc-1 is the instruction's address)
sed -i 's|^  i = \*(pc++); \\$|  i = *(pc++); k6_trace(L, ci, base, pc - 1, i, k); \\|' "$out"/src/lvm.c
sed -i 's|^#include "lvm.h"$|#include "lvm.h"\n#include "k6trace.h"|' "$out"/src/lvm.c
grep -q k6_trace "$out"/src/lvm.c && grep -q 'include "k6trace.h"' "$out"/src/lvm.c
make -s -C "$out"/src linux-noreadline CC=gcc >/dev/null
mv "$out"/src/lua "$out"/lua-trace
