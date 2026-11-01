# E8: Dependency slices under clang -O2 (Lua 5.4.6)

Reproduce: `./run.sh` (about 15 s on 8 cores). It fetches `lua-5.4.6.tar.gz` from lua.org and checks
sha256 `7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88`, which matches the
value published on lua.org/ftp/ (`fetch-lua.sh`). Outputs: `results/<file>.csv` (one row per target), `summary.txt`.
Baseline slices are written to `slices/<file>/` and the preprocessed TUs to `tu/` (both gitignored).

## Common setup
- Compiler: Ubuntu clang 18.1.3 (`clang-18`). Preprocessing uses the flags from `src/Makefile` for `make linux`:
  `-std=gnu99 -O2 -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -DLUA_USE_READLINE -E -P`. `-O2` is passed to `-E` so that `__OPTIMIZE__`
  matches the compile. Compile: `-std=gnu99 -O2 -fno-pic -w -S`. For lparser, compiling the `.i` gives the same asm as compiling
  `lparser.c` directly, apart from `.file`/`.ident` (checked once by hand).
- Targets: the `.globl` functions that each full-TU compile emits. Static functions are excluded and counted.
- Splitter: E5's tokenizer and splitter (imported from `../e5-dependency-slices/slice.py`), with two fixes made in `slice.py`:
  1. E5 named `static void __attribute__((noreturn)) f(...)` as "?". Function names are now recomputed with `__attribute__((…))` skipped.
  2. E5 did not recognize parenthesized declarators such as `extern lua_Unsigned (lua_rawlen) (...)` from lua.h. It missed the declared
     name and treated the return type as the name. Before this fix, 52 targets (mostly in lauxlib) had slices that failed to compile and counted as 100%.
     After it, no slice fails to compile.
- Slice at depth d: the bodies of the target and of every TU function within d call levels, where an edge is any reference to a
  TU function name in a body. Every other referenced TU function appears as its header plus `;` at its definition's position. Context
  declarations are taken to a fixpoint, and everything is emitted in original order. d increases until there is a match or the callee closure
  is exhausted. A target that never matches counts as 100%.
- Comparison: the function's text runs from `.globl`/`.p2align`/`.type` to `.size`. To that the script appends the data blocks it
  references, extracted by label and followed transitively: jump tables, `.LCPI`, `.L.str`, and local objects, each with its section line.
  Private labels (`.L…`, plus the `BBn_m` names in clang's loop comments) are renamed bijectively in order of first occurrence.
  Only `.loc`, `.file` and `.cfi_*` lines are stripped. Comments are kept. Outside quoted strings, runs of whitespace are collapsed to one space.
  This was added for the all-files run: renaming changes label lengths, and clang pads comments to a column. Without it, `lua_pushcclosure` differed
  only in that padding. Everything from a line's first `"` onward is compared verbatim.
- Comparator fix (after a verifier finding): the symbol regex used to let a name start with `$`. So `movl $.L.str.N, %esi` resolved to
  `$.L.str.N`, which never matched a data label, and string contents were never compared. Whitespace collapsing also reached inside strings.
  Both are fixed: the leading `$` is no longer part of the name, and quoted text is kept verbatim. `run.sh` now runs `selftest.py` first. For
  luaL_argerror, luaL_checkoption and luaO_tostring, it flips one byte of a referenced `.asciz`, turns one space inside such a string into two spaces and into a tab, and changes one
  `movq` to `movl`, and it asserts that the comparator rejects all four. luaO_tostring has no string with a space, so its whitespace cases are skipped,
  and `run.sh` requires that they ran somewhere. With the old regex, the byte flip was accepted. With the old whitespace collapsing, the
  two-space mutation was accepted. Both were checked. Re-running after the fix changed
  no result: every number below is identical to the run before it.
- Controls (measured, per file): recompiling the full TU, and recompiling it after re-joining it from the splitter's declarations, both reproduce
  every emitted function in all 32 files (for example lparser 23/23 and lapi 84/84).

## 1. lparser.c alone (N = 2; first run, re-measured after the splitter fixes)
| target | smallest d | bodies | % of TU | inlined into it in full TU |
|---|---|---|---|---|
| luaY_nvarstack | 2 (= its full callee depth) | 3 | **6.59%** | reglevel → getlocalvardesc |
| luaY_parser | 5 (of callee depth 9) | 60 of 96 | **46.96%** | mainfunc → open_func, statlist, setvararg, … |

- Median **26.77%**. The first run reported 7.08%, 47.45% and 27.27%. The declarator fix shrank the context slightly; d did not change.
- Why `luaY_parser` needs d = 5 (measured with `-Rpass=inline`, see `summary.txt`): at d = 4, `close_func` has one caller in the slice.
  clang therefore inlines it into `mainfunc` (cost −14265, the bonus for the last call to a static). In the full TU it declines (cost 730 > 225)
  because of a second caller, `body`, whose definition enters only at d = 5. **Inlining depends on the callee's other callers**, which a
  callee-depth slice does not contain.
- With N = 2, this run could not decide the rule.

## 2. All 32 core and library files, each as its own TU (lua.c and luac.c excluded; measured)
- **342 targets** in 30 files. lctype.c and lopcodes.c emit no extern functions. 553,553 TU tokens in total.
- **714 static functions excluded**, all defined in the Lua `.c` files themselves. 299 of them are emitted standalone in the full-TU asm,
  and the rest are inlined everywhere.
- **Median smallest-matching slice: 6.46% of its TU. p90: 22.59%.** Mean 14.11%.
- Second denominator, excluding system headers (from the same TU preprocessed with line markers; files under `/` count as system).
  185,522 of the 553,553 TU tokens are system-header tokens.
  - Whole slice over non-system TU tokens: **median 8.39%, p90 30.40%**. This reproduces the verifier's figure.
  - System-header tokens removed from both the slice and the TU: median 8.17%, p90 30.11%.
  Either way the median stays ≤ 10%.
- Median absolute slice size: **1,457 tokens**. Never-matching targets count as their whole TU.
- Distribution of d: {0: 173, 1: 98, 2: 38, 3: 4, 5: 4, never: 25}. So 79% match at d ≤ 1, and 7.3% never match short of the full TU.

Per-file medians (n = targets; from `summary.txt`):

| file | n | median | p90 | file | n | median | p90 |
|---|---|---|---|---|---|---|---|
| lapi | 81 | 6.19% | 7.56% | lmathlib | 1 | 6.08% | 6.08% |
| lauxlib | 45 | 1.50% | 6.14% | lmem | 7 | 14.12% | 18.27% |
| lbaselib | 1 | 4.47% | 4.47% | loadlib | 1 | 12.09% | 12.09% |
| lcode | 37 | 6.43% | 100% | lobject | 10 | 4.47% | 12.19% |
| lcorolib | 1 | 3.55% | 3.55% | loslib | 1 | 2.63% | 2.63% |
| ldblib | 1 | 3.75% | 3.75% | lparser | 2 | 26.77% | 46.96% |
| ldebug | 21 | 5.92% | 13.22% | lstate | 16 | 7.92% | 15.97% |
| ldo | 21 | 7.25% | 11.99% | lstring | 11 | 11.44% | 22.59% |
| ldump | 1 | 34.40% | 34.40% | lstrlib | 1 | 2.81% | 2.81% |
| lfunc | 11 | 14.12% | 17.03% | ltable | 15 | 8.67% | 100% |
| lgc | 11 | 6.88% | 100% | ltablib | 1 | 3.19% | 3.19% |
| linit | 1 | 5.86% | 5.86% | ltm | 14 | 10.59% | 11.99% |
| liolib | 1 | 8.52% | 8.52% | lundump | 1 | 41.27% | 41.27% |
| llex | 7 | 10.98% | 13.08% | lutf8lib | 1 | 3.30% | 3.30% |
| lzio | 3 | 14.76% | 17.09% | lvm | 17 | 2.68% | 100% |

The 25 targets that never match: lua_setfield, luaC_changemode, luaC_step, luaC_fullgc, lua_getinfo, luaG_callerror, luaH_next,
luaH_getstr, luaH_get, luaL_traceback, luaL_buffinitsize, luaK_int, luaK_exp2nextreg, luaK_exp2RK, luaK_storevar, luaK_goiftrue,
luaK_goiffalse, luaK_prefix, luaK_infix, luaK_posfix, luaK_fixline, luaV_lessthan, luaV_lessequal, luaV_execute, lua_newstate.
In the case inspected (`luaK_int` at its deepest slice), the slice inlines more than the full TU does. It inlines `addk`, so its asm calls `luaH_get`
directly, whereas the full TU emits `callq addk`, because `addk` has other callers there. This is the same direction as `close_func`.

### Add-callers variant (measured)
At each d, the slice also includes the bodies of all callers of every static callee already in it. This is one step, not a fixpoint.
- Never-matching targets drop from **25 to 7**: 18 of 25 are fixed, for example `lua_setfield` (d = 1, 39%), `luaL_traceback` (d = 2, 13%) and
  `luaL_buffinitsize` (d = 1, 4.8%).
- Of the 169 targets that need d > 0, **29 match at a smaller d**, and none gets worse. 20 get a smaller slice.
- The median rises slightly, to 7.45% (p90 36.56%), because the extra caller bodies cost tokens on targets that already matched.
- So the close_func-style effect (static callees' other callers) explains most of the never-match tail (18 of 25). It barely affects the median,
  which is set by targets that match at d ≤ 1.

## Decision
The rule: median ≤ 10% means a dependency-bounded slice survives -O2. Over 342 targets the **median is 6.46%, so the slice
survives -O2**, and an edit-based test is worth running next. This reverses the N = 2 lparser reading (26.77%), which was not representative.
Excluding system headers, the median is 8.39% (p90 30.40%), which is closer to the bar but still under it.
The caveat is the tail. p90 is 22.6%, and 7.3% of targets need the whole TU under callee-only slicing. Most of that tail needs
caller information for static callees (call-site counts), which is the ThinLTO-summary-style fact.

## Estimated / caveats
- Attributing the tail to caller-count inlining is directly shown for `close_func` (remarks) and `luaK_int` (asm diff). For the other 17 fixed targets
  it is inferred from the add-callers variant, and the 7 that are still never matching were not diagnosed.
- Call edges count any reference to a function name, including address-taking. Function pointers in context declarations (for example
  `luaL_Reg` tables) are not followed as edges. This mainly matters for library `luaopen_*` targets, and all 10 of them matched (d ≤ 2, ≤ 12.1%).
- The per-file medians for files with n = 1 are single points.
- Whitespace collapsing goes beyond the plan's "strip only debug directives". It removes formatting only and drops no line or token.
