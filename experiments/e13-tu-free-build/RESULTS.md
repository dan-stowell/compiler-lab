# E13 results: TU-free build of Lua 5.4.6

Every number here is **measured** by `run.sh` on this VM (clang 18, perf 6.8.12, taskset -c 2). Nothing is estimated. Lines marked
*interpretation* are my reading of the numbers. Raw outputs are in `results/`, and the full log is `results/run.log`.

## Setup
- Lua 5.4.6: sha256 7d5ea1b9…ad88, verified by `fetch-lua.sh`, which is reused from E8.
- lua-5.4.6-tests: sha256 `63ed5f5bcfd15dfd2f72e04c2f8c10585bb8de9e558df62b335cb9e5c1a4ef34`, which matches lua.org/tests/. Fetched by `fetch-tests.sh`.
- Flags are exactly those of the Makefile's `linux-readline` target, with CC=clang-18:
  `-std=gnu99 -O2 -Wall -Wextra -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -DLUA_USE_READLINE`.
  Link flags are `-lm -Wl,-E -ldl -lreadline`.
- **base**: lua.o plus the CORE_O and LIB_O objects, linked in Makefile order as an explicit object list (no liblua.a).
- **shuf1–3**: the same objects, linked in 3 seeded random orders (`results/link-orders.txt`).
- **tufree**: for each of the 19 core files in the plan, `tufree.py split` writes one source per extern target.
  - Each source is the file's preprocessed text with every other target's body replaced by its header plus `;`.
  - Each source is compiled with the unchanged flags.
  - `objcopy --keep-global-symbol` then leaves only the target global. The first object of each file also keeps that file's data
    globals global: `lua_ident` and `luaT_typenames_`. In the other objects, those data globals become local const copies.
  - The resulting objects are linked with the unchanged objects for lctype, lopcodes, the libraries and lua.o. That is 331 split objects.
  - `run.sh` verifies the global symbols of every object.

## Safety check (amended rule)
- 0 non-const file-scope or function-local statics.
- 17 address-taken static functions in 6 files.
  - 15 are reachable from exactly one target, so they stay static.
  - `boxgc` (4 lauxlib targets) and `dothecall` (5 lgc targets) become single hidden globals, `__tuf_lauxlib_boxgc` and
    `__tuf_lgc_dothecall`. Each is defined in one object, and the other objects get a hidden prototype (`results/safety.txt`).
- Checker false-positive fixes, made before the amendment:
  - struct-member uses such as `fs->freereg` are not counted as address-taking;
  - a local that shadows a static, such as `int reglevel = …` in lparser, is not counted.

## Targets and slices
- There are 331 targets. Per file: lapi 81, lauxlib 45, lcode 37, ldebug 21, ldo 21, lvm 17, lstate 16, ltable 15, ltm 14, lfunc 11,
  lgc 11, lstring 11, lobject 10, llex 7, lmem 7, lzio 3, lparser 2, ldump 1, lundump 1.
- Slice size, measured E8-style as the target body plus the bodies of the statics it reaches:
  - share of the file's TU tokens: median 1.57%, p90 7.10%, max 69.87%;
  - absolute: median 338 tokens, p90 1,531;
  - statics reached per target: median 1, max 95.
- Static functions are duplicated: 25 functions have more than one copy in the tufree binary, 96 extra copies in total.
  Function bytes are 185,334 in base and 212,725 in tufree (1.148x).

## Correctness
- Basic mode (`lua -e"_U=true" all.lua`): both base and tufree reach `final OK !!!` with exit 0. The baseline fails nothing, so no
  tests are excluded.
- All 5 benchmark outputs are byte-identical across all 5 binaries.
- *Caveat:* passing the basic-mode tests supports this Lua configuration only. It does not show general correctness.

## Performance
Method:
- The 5 benchmarks are in `bench/`: fib(34), binary-trees(14), spectral-norm(400), n-body(200k steps), and a string-ops loop.
- Each measurement is `perf stat -r 10` of `instructions:u,cycles:u`. Each (benchmark, variant) gets 3 rounds, and the variants run
  interleaved within each round.
- Two intervals are reported:
  - **CI_bench** is the 95% t-interval of the mean log ratio across the 5 benchmarks (df=4). The decision uses this one.
  - **CI_meas** is a 95% interval from round-to-round measurement noise only.

| metric | tufree / base, geomean | CI_bench | CI_meas | per benchmark (bt, fib, nbody, spectral, strings) | noise floor (shuf1-3 geomeans) |
|---|---|---|---|---|---|
| instructions | **1.0008** | [0.9777, 1.0245] | [0.9996, 1.0021] | 1.016, 1.024, 0.980, 0.997, 0.988 | 1.0004, 1.0002, 0.9998 (max 0.04%) |
| cycles | 1.0613 | [0.9677, 1.1640] | [0.910, 1.238] | 1.155, 1.030, 1.002, 1.143, 0.988 | 1.016, 1.034, 1.049 (max 4.9%) |

- **Decision (plan rule, instructions):** the tests pass and the interval [0.978, 1.025] lies wholly at or below 1.03. **TU-free
  compilation is viable at no measurable cost for this program.**
- Cycles on this VM are noisy:
  - the shuffled baselines alone move up to 4.9% in geomean, and up to 15% on binary-trees;
  - the cycles interval straddles both 1.03 and 1.10, so by itself it is **inconclusive**;
  - *interpretation:* the 6% cycles geomean is not distinguishable from layout noise.
- The overall instruction geomean is flat, but individual benchmarks move by up to ±2.4% (fib +2.4%, binary-trees +1.6%, n-body
  −2.0%). Measurement noise on each of these is well under 0.3%. These are real per-benchmark effects in both directions.

## Lost inlining
From `-Rpass=inline` (`results/inlining.txt`):
- Inlined into `luaV_execute` in the full lvm TU but not in its TU-free slice: `luaV_idiv`, `luaV_mod`, `luaV_modf`, `luaV_shiftl`,
  `luaV_tointegerns`.
- Inlined only in the slice: `lessthanothers`, `lessequalothers`.
- `luaV_execute` shrinks from 15,865 to 13,866 bytes.

Other size changes:
- `llex` is no longer a standalone function. It is inlined into the separate objects for `luaX_next` and `luaX_lookahead` (~4 KB each).
- `propagatemark`, `getobjname` and `addk` now have multiple copies.

*Interpretation:* the loss is real at the code level, but the benchmarks spend little time in the arithmetic slow paths it affects.
