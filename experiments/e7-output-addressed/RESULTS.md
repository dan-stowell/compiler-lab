# E7: Output-addressed asm over chibicc history

Reproduce: `./run.sh` clones chibicc into `./chibicc`, builds HEAD 90d1f7f into `./build`, compiles into `./work` (all three are gitignored) and writes `ripple.csv` and `summary.txt`. A cold run takes about 25 s wall. Compiles are cached in `work/asm` by key, so delete `work/` to force a cold run.

## Setup
- Compiler: chibicc built once at HEAD 90d1f7f (`build/chibicc`), used for every commit. It finds system headers in `build/include` (HEAD's copy) and `/usr/include`.
- Inputs: the 316 first-parent commits reachable from 90d1f7f, so 315 transitions, each compared with its first parent. The inputs are all `.c` files outside `test/`, the same set E2 used.
- Per file, the script writes the `.c` file and every non-test `.h` file in that commit's tree into a temp dir using `git cat-file`. It runs `chibicc -S` there, in parallel with `xargs -P8` (`compile.sh`).
- Cache key: sha256 of the path, the `.c` blob and every `(path, blob)` pair for the tree's `.h` files. A file is recompiled only when it or a header changes. There were 1034 distinct compiles in total.
- Splitting (`e7.py`): a function is its own `.globl NAME`/`.local NAME` line plus the lines from `.type NAME, @function` up to the next `.globl`/`.local`/`.text`/`.data`/`.bss`/`.section`. Including the visibility line means a static↔extern change counts as a change; this was fixed after the verifier's review (see 1e1ea39, bf6ff92, 8f6f792). Its identity is (file, name). Added functions count as changed, and removed functions are counted separately. The denominator is the number of functions in the current commit, as in E2.
- Variants:
  - **raw**: the function's lines exactly as emitted.
  - **normalized**: drop `.loc`/`.file` lines. Renumber `.L..N` (string literals, static locals, case/goto labels) and `.L.kind.N` (codegen counter labels such as `.L.else.N`) in order of first appearance within the function, with one numbering for each of the two namespaces.
  - **noloc** (diagnostic): drop `.loc`/`.file` lines only, with no renumbering.
  - **labels-only** (diagnostic): renumber labels only, keeping `.loc`/`.file`.
  - **normalized+literals** (correctness check): normalized, plus the renumbered contents of every `.L..N` data object the function references, followed transitively. Normalized alone cannot see an edited string literal, because literals sit in `.data` outside the function.

## Results (measured, 315 transitions)
| metric | median | p90 | mean |
|---|---|---|---|
| raw asm hash changed | **26.4%** | 60.3% | 30.0% |
| noloc (diagnostic) | 19.4% | 49.2% | n/a |
| labels-only (diagnostic) | 15.3% | 52.3% | n/a |
| normalized asm hash changed | **2.1%** | 16.9% | 6.0% |
| normalized+literals | 2.1% | 16.9% | 6.0% |
| E2 body hash (source) | 1.5% | 7.0% | 4.1% |
| E2 closure key (source) | 21.6% | 40.9% | 20.3% |

- Commits included: 315 of 315 transitions. Excluded: 0, because HEAD chibicc compiled every historical file, including the earliest single-file `main.c` commits.
- Compile CPU total (user+sys, summed over the 1034 compiles): **30.0 s** (cold rerun 30.4 s). Wall time with `-P8` was 6 s.
- Functions at HEAD: 324. Of these, 27 are `__va_arg_{fp,gp,mem}`, which come from HEAD's `include/stdarg.h` and are emitted in each of the 9 files. 324 − 27 = 297, which matches E2's count. With those 27 left out (ad hoc rerun, measured), the medians are raw 28.5%, normalized 2.3% (p90 20.0%) and normalized+literals 2.3%. The decisions do not change.
- Literal contents change the per-commit count in only 2 of 315 transitions. String-literal edits almost always come with a code change in the same function.
- Normalized ripple stays at or below 3% in 192 of 315 transitions.
- Per-commit comparison with E2 (measured): the median of normalized minus E2's body fraction is +0.26 points. Normalized exceeds body by more than 5 points in 54 transitions, and exceeds E2's closure fraction in 9.
- Spot-check (measured), e0bf168 "Add long double": normalized changes 36%, against 5% for body. In `assign_lvar_offsets`, the diff is a type-kind constant: inserting `TY_LDOUBLE` into the `chibicc.h` enum renumbers the enum values that come after it (`cmp $13` becomes `cmp $15`). This is a real change in output that comes in through a header, not compiler noise. The tail of the normalized distribution (p90 16.9% against E2 body's 7.0%) is made of commits like this: enum and struct-layout edits in `chibicc.h`.

## Decomposition of raw ripple (measured)
The split between the two steps depends on which one runs first, because a function often changes for both reasons at once:

| order | first step | second step |
|---|---|---|
| `.loc` first | strip `.loc`/`.file`: 26.4% → 19.4% (7.0 pts) | renumber labels: 19.4% → 2.1% (17.3 pts) |
| labels first | renumber labels: 26.4% → 15.3% (11.1 pts) | strip `.loc`/`.file`: 15.3% → 2.1% (13.2 pts) |

- Neither step alone gets close to 3%; **both are needed**. Labels account for 11–17 points and `.loc` for 7–13, depending on order.
- `.loc`/`.file`: line numbers shift whenever code above a function moves, and `.file` numbering shifts when the include set changes.
- Labels: chibicc numbers every label from global counters for the whole file (`count()` for `.L.else.N`/`.L.end.N`, and the unique-name counter for `.L..N`). Adding one `if` or one string literal early in a file renumbers every function after it.

## Decisions
1. **Normalized median 2.1% ≤ 3%: output addressing removes closure ripple.** The 21.6% closure median from E2 falls to about the source-body level (1.5%), because a function's asm depends on callee prototypes and types, not on callee bodies. This holds for normalized+literals too (2.1%).
2. **Raw − normalized = 24.4 points > 5: hidden global counters cause ripple.** The file-global label counters account for 11–17 of those points and `.loc` line numbers for 7–13, depending on which is removed first; both must go. Raw per-function asm hashes from stock chibicc ripple more than E2's closure keys did (26.4% vs 21.6%). So output addressing only works if the compiler emits labels local to each function (or they are renumbered afterwards) and keeps debug line info out of the hashed text.

## Caveats (estimated, not measured)
- Named static and global data (e.g. `cast_table`, `base_file`) is in no function's hash: it is neither a function nor a `.L..N` object reachable through normalized+literals. A change to such an initializer is not counted here. A real output-addressed build would need separate keys for data objects.
- Normalized hashes treat function asm as alpha-equivalent under label renaming. For linking this is safe only if labels are made function-local, for example by prefixing the function name. That is an assumption here and was not tested by linking.
- `.loc` was dropped, not kept in a position-independent form. A real build needs debug info carried in a separate, non-hashed side table.
