#!/bin/sh
# E13: TU-free build of Lua 5.4.6 (see journal/cycle5-plan.md, E13, amended).
#   1. baseline: every .c compiled with the Makefile's linux flags (clang-18 -O2),
#      linked in Makefile order; plus 3 links with shuffled object order (noise floor)
#   2. safety check on the 19 split core files (tufree.py check)
#   3. TU-free build: one object per extern target per core file (tufree.py split),
#      objcopy keeps only that target (plus owned promoted statics / data) global
#   4. lua-5.4.6-tests in basic mode
#   5. 5 benchmarks, outputs checked, perf stat -r 10 interleaved, taskset
#   6. analysis (analyze.py), slice stats, inlining diagnostics
set -eu
cd "$(dirname "$0")"
CC=${CC:-clang-18}
JOBS=${JOBS:-$(nproc)}
CPU=${CPU:-2}
ROUNDS=${ROUNDS:-3}
PERF=${PERF:-$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1)}
# src/Makefile: CFLAGS= -O2 -Wall -Wextra -DLUA_COMPAT_5_3 $(SYSCFLAGS), CC= gcc -std=gnu99;
# linux-readline: SYSCFLAGS="-DLUA_USE_LINUX -DLUA_USE_READLINE" SYSLIBS="-Wl,-E -ldl -lreadline";
# LIBS= -lm $(SYSLIBS); lua: $(CC) -o $@ $(LDFLAGS) lua.o liblua.a $(LIBS)
FLAGS="-std=gnu99 -O2 -Wall -Wextra -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -DLUA_USE_READLINE"
LIBS="-lm -Wl,-E -ldl -lreadline"
CORE_O="lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio"
LIB_O="lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit"
SPLIT="lvm ldo ltable lgc lapi lstring lfunc ltm lobject ldebug lstate lauxlib lcode lparser llex lzio lmem lundump ldump"
$CC --version | head -1; $PERF --version
sh ./fetch-lua.sh lua-5.4.6
sh ./fetch-tests.sh lua-5.4.6-tests
rm -rf build && mkdir -p build/base build/tu build/split build/bin results
S=lua-5.4.6/src

echo "== 1. baseline"
for b in $CORE_O $LIB_O lua; do echo $b; done | xargs -P "$JOBS" -I{} $CC $FLAGS -c $S/{}.c -o build/base/{}.o
objs() { for b in "$@"; do printf 'build/base/%s.o ' $b; done; }
$CC -o build/bin/base build/base/lua.o $(objs $CORE_O $LIB_O) $LIBS
for seed in 1 2 3; do
  order=$(python3 -I -c 'import random,sys; l=sys.argv[2:]; random.Random(int(sys.argv[1])).shuffle(l); print(" ".join(l))' \
          $seed lua $CORE_O $LIB_O)
  echo "shuf$seed: $order" >> results/link-orders.txt
  $CC -o build/bin/shuf$seed $(objs $order) $LIBS
done

echo "== 2. safety check"
for b in $SPLIT; do
  $CC $FLAGS -E -P $S/$b.c -o build/tu/$b.i
  nm -g --defined-only build/base/$b.o | awk '$2=="T"{print $3}' > build/tu/$b.targets
  nm -g --defined-only build/base/$b.o | awk '$2!="T"{print $3}' > build/tu/$b.data
done
for b in $SPLIT; do python3 -I tufree.py check build/tu/$b.i build/tu/$b.targets; done \
  > results/safety.txt || true
cat results/safety.txt
if grep -q '^VIOLATION' results/safety.txt || ! [ "$(grep -c 'violation(s)' results/safety.txt)" = 19 ]; then
  echo "SAFETY CHECK FAILED (see results/safety.txt)" >&2; exit 1
fi

echo "== 3. TU-free build"
for b in $SPLIT; do python3 -I tufree.py split build/tu/$b.i build/tu/$b.targets build/split/$b; done
# compile every slice with the unchanged flags (warnings about unused statics go to a log)
for b in $SPLIT; do for c in build/split/$b/*.c; do echo $c; done; done | \
  xargs -P "$JOBS" -I{} sh -c '$0 $1 -c "$2" -o "${2%.c}.o" 2>>"${2%.c}.log"' "$CC" "$FLAGS" {}
tufree_objs=""
for b in $SPLIT; do
  first=1
  while read -r t owned; do
    keep="$t $owned"
    [ $first = 1 ] && keep="$keep $(cat build/tu/$b.data)"   # data globals (lua_ident, luaT_typenames_) once
    first=0
    o=build/split/$b/$t.o
    objcopy $(for k in $keep; do printf -- '--keep-global-symbol=%s ' $k; done) $o
    got=$(nm -g --defined-only $o | awk '{print $3}' | sort | tr '\n' ' ')
    want=$(for k in $keep; do echo $k; done | sort | tr '\n' ' ')
    [ "$got" = "$want" ] || { echo "global symbols of $o: '$got' != '$want'" >&2; exit 1; }
  done < build/split/$b/keep.txt
done
link=""
for b in lua $CORE_O $LIB_O; do
  if [ -f build/split/$b/keep.txt ]; then
    link="$link $(awk -v d=build/split/$b '{printf "%s/%s.o ", d, $1}' build/split/$b/keep.txt)"
  else
    link="$link build/base/$b.o"
  fi
done
$CC -o build/bin/tufree $link $LIBS
python3 -I stats.py build/split > results/slices.txt
cat results/slices.txt

echo "== 4. tests (basic mode)"
for v in base tufree; do
  ( cd lua-5.4.6-tests && ../build/bin/$v -e"_U=true" all.lua ) > results/tests-$v.log 2>&1 \
    && st=0 || st=$?
  if [ $st = 0 ] && grep -q "^final OK" results/tests-$v.log; then echo "$v: pass (exit 0, final OK)"
  else echo "$v: FAIL (exit $st)"; tail -n 5 results/tests-$v.log; fi
  grep "total time" results/tests-$v.log
done | tee results/tests.txt

echo "== 5. benchmarks"
VARIANTS="base shuf1 shuf2 shuf3 tufree"
for f in bench/*.lua; do
  n=$(basename $f .lua)
  ./build/bin/base $f > results/out-$n.base
  for v in $VARIANTS; do
    ./build/bin/$v $f | cmp - results/out-$n.base || { echo "output differs: $v $n" >&2; exit 1; }
  done
done
echo "outputs identical for all variants" | tee results/bench-outputs.txt
rm -f results/perf-*.csv
for r in $(seq 1 $ROUNDS); do
  for f in bench/*.lua; do
    n=$(basename $f .lua)
    for v in $VARIANTS; do
      taskset -c $CPU $PERF stat -x, -e instructions:u,cycles:u -r 10 -o results/tmp.csv \
        ./build/bin/$v $f > /dev/null
      grep -v '^#' results/tmp.csv | grep . | sed "s/^/$r,$n,$v,/" >> results/perf.csv
    done
  done
done
rm -f results/tmp.csv

echo "== 6. analysis"
tres=fail; grep -q "^tufree: pass" results/tests.txt && grep -q "^base: pass" results/tests.txt && tres=pass
python3 -I analyze.py results/perf.csv $tres | tee results/analysis.txt
# inlining into luaV_execute: full lvm TU vs its TU-free slice
for src in build/tu/lvm.i build/split/lvm/luaV_execute.c; do
  $CC $FLAGS -w -c -o /dev/null -Rpass=inline "$src" 2>&1 | \
    sed -n "s/.*remark: '\([A-Za-z_0-9]*\)' inlined into 'luaV_execute'.*/\1/p" | sort -u > results/inl-$(basename $src | sed 's/\..*//').txt
done
{ echo "callees inlined into luaV_execute in the full TU but not in the TU-free slice:"
  comm -23 results/inl-lvm.txt results/inl-luaV_execute.txt
  echo "inlined only in the TU-free slice:"
  comm -13 results/inl-lvm.txt results/inl-luaV_execute.txt
  echo
  python3 -I sizes.py build/bin/base build/bin/tufree
} | tee results/inlining.txt
