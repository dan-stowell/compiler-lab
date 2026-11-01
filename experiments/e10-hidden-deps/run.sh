#!/bin/sh
# Reproduce E10: hidden dependencies of learned slices over lua/lua history.
# WINDOW = number of newest first-parent commits (touching core .c/.h) to use.
set -eu
cd "$(dirname "$0")"
CC=${CC:-clang-18}; export CC
JOBS=${JOBS:-$(nproc)}
WINDOW=${WINDOW:-1500}
PIN=8cb93bada561977eb370819e39d7d644e5fd0a3f   # lua/lua HEAD used (2026-10-16)
OUT=${OUT:-work}
$CC --version | head -1
[ -d lua ] || git clone -q https://github.com/lua/lua.git lua
git -C lua cat-file -e "$PIN^{commit}"
mkdir -p "$OUT"
git -C lua log --first-parent --format=%H "$PIN" -- ':(glob)*.c' ':(glob)*.h' \
    ':(exclude)onelua.c' ':(exclude)ltests.c' ':(exclude)lua.c' ':(exclude)luac.c' \
  | head -n "$WINDOW" | tac > "$OUT/commits.txt"
echo "commits: $(wc -l < "$OUT/commits.txt") ($(head -1 "$OUT/commits.txt") .. $(tail -1 "$OUT/commits.txt"))"
# Comparator self-test (E8's, incl. the string fix) on lauxlib at the pinned commit.
mkdir -p "$OUT/selftest"
git -C lua archive "$PIN" | tar -x -C "$OUT/selftest" --wildcards '*.c' '*.h'
$CC -std=c99 -O2 -DLUA_USE_LINUX -fno-stack-protector -fno-common -fno-pic -w -S \
    "$OUT/selftest/lauxlib.c" -o "$OUT/selftest/lauxlib.s"
python3 -I ../e8-slices-O2/selftest.py "$OUT/selftest/lauxlib.s" luaL_argerror | tee "$OUT/selftest.txt"
# 1. token-diff-only pass (preprocess every core TU at every commit) and estimate of n
[ -f "$OUT/tok.tsv" ] || python3 -I e10.py tokpass lua "$OUT/commits.txt" "$OUT" "$JOBS"
python3 -I e10.py estimate "$OUT" | tee "$OUT/estimate.txt"
# 2. the experiment, one independent chain per TU. lcode is the long pole (many
# never-matching targets are re-learned at every lcode change), so its re-learning
# runs in its own process pool next to the other chains.
rm -rf "$OUT/events" "$OUT/mismatch"
cut -f3 "$OUT/tok.tsv" | sort -u > "$OUT/files.txt"
LEARN_JOBS=$JOBS python3 -I e10.py chain "$OUT" lcode & lpid=$!
grep -vx lcode "$OUT/files.txt" | xargs -P "$JOBS" -I{} python3 -I e10.py chain "$OUT" {}
wait $lpid
{ cat "$OUT/selftest.txt" "$OUT/estimate.txt"; python3 -I e10.py summarize "$OUT"; } | tee summary.txt
