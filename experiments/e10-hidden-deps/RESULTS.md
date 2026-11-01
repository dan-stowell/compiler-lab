# E10: Hidden dependencies of learned slices (lua/lua history, clang -O2)

Reproduce: `./run.sh`. Measured on 8 cores: 29 min wall for the chains, plus about 4 min for the token pass on a fresh `work/`.
lvm is the long pole. The script clones `https://github.com/lua/lua.git` into `lua/` and requires the pinned HEAD
`8cb93bada561977eb370819e39d7d644e5fd0a3f` (2026-10-16).
Outputs:
- `summary.txt`: every number below.
- `work/` (gitignored):
  - `tok.tsv`: the token pass.
  - `estimate.txt`.
  - `events/<file>.jsonl`: one line per hit, miss, learn, failure or determinism check.
  - `mismatch/<file>/<idx>_<target>.{N,N1}.s`: the normalized asm on both sides.

`WINDOW=n ./run.sh` changes the number of newest commits used (default 1500).

## Setup (as in the plan)
- History: the first-parent commits of lua/lua that touch root `*.c`/`*.h`, excluding `onelua.c`, `ltests.c`, `lua.c` and `luac.c`.
  - The window is the newest **1500** of the 5163 such commits (2014-05-15 .. 2026-10-16), which gives 1499 transitions N -> N+1.
  - Each core `.c` at each commit is its own TU. The set of files follows the tree, so lbitlib comes and goes.
- Flags are pinned for the whole window. They are the developer `makefile`'s linux settings at the pinned HEAD
  (`CFLAGS= -Wall -O2 $(MYCFLAGS) -fno-stack-protector -fno-common`, `MYCFLAGS= -std=c99 -DLUA_USE_LINUX`), with warnings and `TESTS` dropped:
  - preprocess: `clang-18 -std=c99 -O2 -DLUA_USE_LINUX -fno-stack-protector -fno-common -E -P`
  - compile: `clang-18 -std=c99 -O2 -fno-stack-protector -fno-common -fno-pic -w -S` (Ubuntu clang 18.1.3)
- Slicer and comparator: E8's `../e8-slices-O2/slice.py`, imported. The comparator includes the string fix.
  - `run.sh` re-runs E8's `selftest.py` on lauxlib at the pinned HEAD, and all 4 mutations are rejected.
  - `slice.py` was refactored so that the TU analysis (`TU`) and `search` can be reused. It also gained an "emit slice at fixed d" mode:
    `slice.py --emit <tu.i> <target> <d>`.
  - E8's `run.sh`, re-run after the refactor, reproduced every committed E8 result exactly (`git status` clean).
- Learning: E8's search. The slice is callee-only, and d is the smallest depth whose slice reproduces the target's normalized full-TU asm.
  If no d matches, the slice is the whole TU ("never").
- Cache key: the token sequence of the slice **re-extracted at N+1 with d frozen from N**, in definition order. Compiler and flags are constant.
  - Targets that were renamed or deleted, or that no longer emit a `.globl`, are misses.
  - Whole-TU targets stay in. Their TU changed, so they always miss.
- Counted hits: only in TUs whose `-E -P` token stream changed between N and N+1.
  - For each counted hit, the full-TU normalized asm at N+1 is compared with N's.
  - d is re-learned only for misses and mismatches.
  - A compile failure breaks a TU's chain. Nothing is counted across it, and every target is re-learned at the next good commit.
- Determinism: every 25th unchanged-TU commit per file is recompiled and compared on every function.
- Parallelism: one independent chain per TU.
  - Learning at a commit is pure in (TU version, target), so the lcode chain runs it in a process pool.
  - This gave events identical to the sequential code for ldo on the 200-commit pilot, and for lcode over the first 603 commits of an interrupted earlier run.

## Token-diff-only pass (estimate of n)
- The estimate is an upper bound on counted hits: the extern definitions at N in TUs whose token stream changed.
  It is 17,035 for the newest 200 transitions and 106,720 for the newest 1000.
- A 200-commit pilot (measured) already gave **n = 10,084 counted hits with 2 mismatches**, both in one commit (c931d86e).
  So the plan's n ≥ 3,000 was reached within 200 commits.
- The window was then widened to 1500 to get more mismatch events for the cause breakdown, still within the ~1 h budget.
  **This goes beyond the plan's stopping point.**

## Results (measured; window of 1500 commits)
| quantity | value |
|---|---|
| changed-TU transitions evaluated | 7,285 |
| **counted hits n** | **63,504** |
| misses | 41,211 (slice tokens changed 32,690; whole-TU targets 8,445; target gone 76) |
| hit rate | **60.64%** (63,504 / 104,715) |
| **mismatches among counted hits** | **22 (0.035%)**; 95% Clopper-Pearson (i.i.d.) [0.022%, 0.052%] |
| commits with >= 1 counted hit | 1,150 |
| commits with >= 1 mismatch | **12 (1.04%)**; 95% CP [0.54%, 1.82%] |
| per-commit mismatch counts (mismatches: commits) | 0: 1138, 1: 9, 2: 2, 9: 1 |
| **commit-clustered bound** (bootstrap over commits, 4000 reps) | 95% interval for the mismatch rate **[0.011%, 0.070%]** |
| determinism spot-checks | 5,613 / 5,613 identical |
| hits by frozen d | 0: 35,855; 1: 20,256; 2: 5,851; 3: 944; 4: 221; 5: 322; 7: 55 |
| mismatches by frozen d | 0: 14; 1: 7; 2: 1 |

Commits with mismatches (window index, commit, mismatches / counted hits):

| index | commit | mismatches / hits | | index | commit | mismatches / hits |
|---|---|---|---|---|---|---|
| 430 | 1a44e822 | 1/168 | | 787 | 8c8a91f2 | 1/61 |
| 623 | 9d28b401 | 1/9 | | 816 | 7696c647 | 1/39 |
| 627 | 49dfaf74 | 1/200 | | 930 | 72a094bd | 2/111 |
| 702 | 75efc672 | 1/146 | | 1200 | 9e99f307 | 1/29 |
| 752 | 34aa0c5b | 1/30 | | 1227 | d862da6d | 1/89 |
| **777** | **4d5de1c1** | **9/27** | | 1356 | c931d86e | 2/250 |

By file: lcode 10, ldo 3, ltm 3, ltable 2, and 1 each in lapi, lauxlib, lgc and lstring.

### Compile failures (recorded, not dropped)
- **245 of the 1500 commits (16.3%) have at least one compile failure.** Per-file counts are in `summary.txt`.
- Preprocessing failed for 144 (commit, file) pairs in **6 commits**. These are broken upstream commits:
  - 4 consecutive commits from 2014-11-02 include `lprefix.h` before it exists.
  - 2 commits from 2015-10 call `luaC_checkGC` with the wrong macro arity, in 6 files.
- The full-TU compile failed for 695 (commit, file) pairs in **239 commits**:
  - **602 come from the pinned `-std=c99`** in 2014-2015 (window indices 0..388). Strict C99 hides POSIX declarations:
    `_longjmp` in ldo (151), `random`/`srandom` in lmathlib (150), and `popen`/`pclose` in liolib and loslib (150 each).
    The makefile of that era set no `-std`, so these failures come from pinning, not from bugs upstream.
  - 66 come from the deprecated lbitlib.c (2018), which is not meant to compile.
  - 27 are broken intermediate states upstream, in lgc, lcode, ltable, lvm, lmathlib, ldump and lundump:
    implicit `lua_checkmemory` or `luai_numinvalidop`, an undeclared `LUA_UNSIGNEDBITS`, and an argument-count mismatch in ldump/lundump.
- No slice failed to compile during learning (0 of 42,101 learn events), and the splitter never failed.

## Causes of the mismatches
**Every mismatch has the same mechanism**: a function outside the slice changed whether it gets inlined.
1. The target calls a function defined in the same TU, which lies **outside the slice**. The slice gives it only as a prototype.
2. When d was learned, the full TU did *not* inline that callee, so a prototype was enough to reproduce the asm.
3. At N+1 something about the callee changed. The full TU now inlines it, or inlines its subtree differently.
4. The target's own slice tokens did not change, so the cache counts a hit.

For each mismatch, `e10.py` records:
- the call targets that appear in or disappear from the asm,
- whether each of them is in the slice,
- whether its body changed,
- its caller counts at N and N+1.

Classes from these diagnostics (measured):

| cause | mismatches | example (inspected by hand) |
|---|---|---|
| out-of-slice callee's **body changed**, and its inlining flipped | **21** | c931d86e (below) and 4d5de1c1 (below) |
| out-of-slice callee's **caller count changed**, body unchanged | **1** | d862da6d (below) |
| header constant or type change outside the slice | 0 | such changes alter the slice's own context tokens, so they show up as misses, not as mismatches |

The three examples:
- **c931d86e**: `luaD_seterrorobj` shrank from a 3-case switch to an if/else. The full TU now inlines it into `luaD_pcall` (d = 0),
  and through that into `luaD_protectedparser` (d = 1).
- **4d5de1c1**: the static `luaK_code` was rewritten, including a changed `savelineinfo` signature. It is now inlined into 9 lcode targets at once.
- **d862da6d**: `luaH_setint` stopped calling `luaH_finishset`, so its callers went from 2 to 1. The full TU then inlined `luaH_finishset`
  into `luaH_set` (d = 0). This is E8's `close_func` effect, here on an extern function.

Notes on the breakdown:
- Direction: in every case the callee went from called at N to inlined at N+1. The reverse case is silent only if an inlined callee's body
  could stay outside the slice, and it cannot: the learned slice had to contain that body to reproduce the asm, so a change to the body changes the key.
- Clustering: one commit (4d5de1c1) accounts for 9 of the 22, because one widely used static helper changed.
- Inspection depth: 3 cases were read by hand, from the asm diff and the commit diff: c931d86e, d862da6d and 4d5de1c1. The class of the other 19
  is **estimated** from the automatic diagnostics (out-of-slice callee with a changed body, whose call appeared or disappeared). Each was not confirmed by hand.

## Decision (plan rule)
The rule is that any mismatch means learned slices are unsound under -O2.
This run found **22 mismatches in n = 63,504 counted hits** (0.035%). The commit-clustered 95% interval is 0.011%-0.070%, and 1.0% of commits with hits had a mismatch.
The 200-commit pilot alone (n = 10,084) had 2.

So **learned callee-depth slices are unsound under -O2.** The hidden dependency is specific: whether a TU-local callee is inlined depends on
**that callee's body and its caller count**. A slice that gives the callee only as a prototype contains neither.
A sound key would add both for every TU-local function that the slice gives only as a prototype: its body tokens (or an inline-cost summary)
and its caller count. This is the ThinLTO-summary-style fact that E8 pointed to.
Keying on bodies alone would have missed 1 of the 22 (d862da6d, a caller-count change).

## Caveats
- The hit rate counts whole-TU ("never") targets as permanent misses (8,445 of the misses), as the plan specifies.
- Flags are pinned to today's makefile. The `-std=c99` pin breaks 2014-2015 ldo, lmathlib, liolib and loslib, so those files contribute only after the chain restarts.
- Call edges are E8's: any reference to a TU function name. The cause classifier sees only the `call`/`jmp` targets in the normalized asm.
- An earlier, incomplete run gave the same 22 mismatches. In that run xargs stopped launching chains after one chain was killed by hand, so it is not reported.

## Independent verification (2026-11-01)

Confirmed.
- E8 reproduces unchanged under the refactored slice.py.
- Four mismatches were reproduced by hand (1227, 1200, 777, 1356). In each, the slice is byte-identical at N and N+1, the full-TU asm differs, and -Rpass shows the out-of-slice callee newly inlined.
- Five random non-mismatch hits were confirmed. No hit has an unchanged TU token stream.
- The 200-commit pilot reproduces (n=10,084, 2 mismatches).

Correction to the cause labels: at 1227 (`luaH_set`) the "caller count changed" label simplifies a two-level effect. `luaH_newkey` stopped being inlined into `luaH_finishset`, which shrank `luaH_finishset` below the inlining threshold for `luaH_set`.
