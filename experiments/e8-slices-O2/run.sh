#!/bin/sh
# Reproduce E8 from scratch: fetch Lua 5.4.6 (sha256-checked), preprocess every
# core and library .c (not lua.c/luac.c) with the Makefile's linux flags, each as
# its own TU, and find each externally visible function's smallest callee-body
# depth that reproduces its clang -O2 asm (plus a variant that also includes the
# callers of included static callees).
set -eu
cd "$(dirname "$0")"
CC=${CC:-clang-18}
JOBS=${JOBS:-$(nproc)}
$CC --version | head -1
sh ./fetch-lua.sh lua-5.4.6
# src/Makefile: CC= gcc -std=gnu99; CFLAGS= -O2 -Wall -Wextra -DLUA_COMPAT_5_3 $(SYSCFLAGS);
# `make linux` -> linux-readline: SYSCFLAGS="-DLUA_USE_LINUX -DLUA_USE_READLINE".
# -O2 is kept for -E so __OPTIMIZE__ matches the compile; -P drops line markers (no -g).
rm -rf tu slices results
mkdir -p tu results
files=$(cd lua-5.4.6/src && ls *.c | grep -vx -e lua.c -e luac.c | sed 's/\.c$//')
for b in $files; do
  $CC -std=gnu99 -O2 -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -DLUA_USE_READLINE -E -P \
      lua-5.4.6/src/$b.c -o tu/$b.i
done
echo "$files" | xargs -P "$JOBS" -I{} \
  python3 -I slice.py tu/{}.i lua-5.4.6/src/{}.c results/{}.csv slices/{} $CC -std=gnu99 -O2 -fno-pic -w
python3 -I summarize.py results/*.csv | tee summary.txt
# Diagnostic: clang's inliner decisions for close_func, full TU vs luaY_parser's d=4 slice.
for f in tu/lparser.i slices/lparser/luaY_parser.d4.c; do
  echo "inline remarks ($f):"
  $CC -std=gnu99 -O2 -fno-pic -w -S -o /dev/null -Rpass=inline -Rpass-missed=inline "$f" 2>&1 \
    | grep -E "^[^ ]* remark: 'close_func' .*'(mainfunc|luaY_parser)'" | sed 's/^[^ ]* remark: /  /'
done | tee -a summary.txt
