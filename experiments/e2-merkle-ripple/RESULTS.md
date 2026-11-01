# E2: Merkle ripple over chibicc history

Reproduce: `./run.sh` (clones chibicc into `./chibicc`, gitignored; takes about 10 s).
Outputs: `ripple.csv` (main run), `ripple-with-tests.csv` (sensitivity run), `summary.txt`, `spotcheck.txt`.

## Setup
- 316 commits reachable from 90d1f7f in first-parent order, which gives 315 transitions. Each commit is compared with its first parent. Files are read with `git cat-file`, with no checkouts.
- Main run: all `.c` files outside `test/`. The `test/*.c` files are inputs to the compiler, not part of it. The sensitivity run includes them.
- Splitter (`ripple.py`): raw tokens with no preprocessing, and comments and whitespace dropped. A function definition is `identifier ( ... ) {` at brace depth 0. Its body tokens run from the end of the previous top-level declaration through the closing `}`, so the return type and signature are included. A preprocessor line is kept as one token, so it affects the hash, but its braces do not count toward depth.
- Function identity is the name, or `file:name` if the name is defined in more than one file. A call is any identifier in the body that names another function in the same commit. Calls to a same-file static resolve to that file's copy. A call to a name that exists only as statics in other files depends on all of those copies.
- Closure key: the call graph is split into SCCs with Tarjan's algorithm. Each SCC gets one hash, computed from all member bodies plus the sorted keys of callees outside the SCC. Every member shares that hash.
- Fractions use the number of functions in the current commit as the denominator, and added functions count as changed. Removed functions are counted separately in `n_removed`.

## Splitter spot-check (measured)
| commit | splitter | grep heuristic | note |
|---|---|---|---|
| 90d1f7f (HEAD) | 297 | 298 | The one extra in grep is a false positive: an `__attribute__((format...))` line above `println`. |
| 0d71737 (#100) | 138 | 138 | Same name sets in every file. |
| 5e0f8c4 (#200) | 229 | 229 | Same name sets in every file. |

The grep heuristic counts lines starting in column 0 that contain `name(` and whose signature ends in `) {`. On these three commits the splitter matches it exactly, apart from the false positive noted above.

## Results (measured, main run, 315 transitions)
| metric | median | p90 | mean |
|---|---|---|---|
| body-hash change fraction | **1.5%** | 7.0% | 4.1% |
| closure-key change fraction | **25.8%** | 49.2% | 23.9% |

- Function count at HEAD is 297. The largest SCC is **76 functions** at HEAD, which is also the maximum over the history. It is the recursive-descent core of `parse.c`: `assign`, `add`, `declarator`, `initializer`, and so on.
- Across all transitions, closure-key changes outnumber body changes by about **9.9x**.
- Only 8 functions were removed across the whole history, in 5 commits.
- Restricted to commits with at least 100 functions (261 of them), the median body change is 1.3% and the median closure change is 22.5% (p90 48%).
- Sensitivity run including `test/`: median closure change is 23.3% (p90 41%), and median body change is 1.8%. The decision is the same.

Closure ripple histogram (measured, number of transitions):
```
         0   13
    (0,5%]   89
   (5,10%]   32
  (10,20%]   12
  (20,30%]   22
  (30,40%]   67
  (40,50%]   56
  (50,75%]   25
 (75,100%]    2
```
The distribution is bimodal. Commits that leave the big parser SCC alone ripple a few percent. Commits that touch any SCC member change the shared SCC hash, and that change propagates to every transitive caller, typically 30 to 50% of all functions.

This was checked over the last 149 transitions (measured, ad hoc, not part of `run.sh`):
- 45 transitions touched a big-SCC body. Their median ripple was 33%.
- The other 104 did not. Their median ripple was 3.2%.

## Decision
The median closure ripple is 25.8%, above the 10% threshold. **Closure keys over-invalidate.** Whole-closure (Unison-style) addressing is not viable for C here. Per the plan, the next step is keys built from prototypes.

## Caveats
- The call relation over-approximates. Any identifier that matches a function name counts as a call, including struct fields or locals with the same name. This can only add edges, so it inflates ripple. Given the 76-member SCC from real mutual recursion, it is unlikely to bring the median under 10%, but that has not been measured.
- Macros are not expanded, so calls hidden inside macros are missed. Function-like macros defined at file level are not split out as functions.
- Identity is by name. A rename shows up as one function added and one removed.
