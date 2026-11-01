# E2: Merkle ripple over chibicc history

Reproduce: `./run.sh` (clones chibicc into `./chibicc`, gitignored; takes about 10 s).
Outputs: `ripple.csv` (main run), `ripple-with-tests.csv` (sensitivity run), `ripple-any-ident.csv` (old call rule), `summary.txt`, `spotcheck.txt`.

## Setup
- 316 commits reachable from 90d1f7f in first-parent order, which gives 315 transitions. Each commit is compared with its first parent. Files are read with `git cat-file`, with no checkouts.
- Main run: all `.c` files outside `test/`. The `test/*.c` files are inputs to the compiler, not part of it. The sensitivity run includes them.
- Splitter (`ripple.py`): raw tokens with no preprocessing, and comments and whitespace dropped. A function definition is `identifier ( ... ) {` at brace depth 0. Its body tokens run from the end of the previous top-level declaration through the closing `}`, so the return type and signature are included. A preprocessor line is kept as one token, so it affects the hash, but its braces do not count toward depth.
- Function identity is the name, or `file:name` if the name is defined in more than one file. A call is an identifier in the body followed by `(`, not after `->` or `.`, that names another function in the same commit. Calls resolve to a same-file static first. A call to a name that exists only as statics in other files depends on all of those copies.
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
| closure-key change fraction | **21.6%** | 40.9% | 20.3% |
| closure-key change, excluding added functions | **20.9%** | 39.6% | 19.5% |

- Function count at HEAD is 297. The largest SCC is **55 functions** at HEAD, which is also the maximum over the history. It is the recursive-descent core of `parse.c`.
- Across all transitions, closure-key changes outnumber body changes by about **8.2x**.
- Only 8 functions were removed across the whole history, in 5 commits.
- The excluding-added variant counts only functions present in both commits, in the numerator and in the denominator.
- Restricted to commits with at least 100 functions (261 of them), the median body change is 1.3% and the median closure change is 16.2% (p90 37.8%).
- Sensitivity run including `test/`: median closure change is 17.9% (p90 36.1%), and median body change is 1.8%. The decision is the same.

Closure ripple histogram (measured, number of transitions):
```
         0   13
    (0,5%]   89
   (5,10%]   32
  (10,20%]   20
  (20,30%]   65
  (30,40%]   64
  (40,50%]   23
  (50,75%]   12
 (75,100%]    2
```
The distribution is bimodal (measured over all 315 transitions; the `big_scc_touched` column in `ripple.csv`):
- 127 transitions changed or added a body in the commit's largest SCC. Their median ripple was **31.6%**. A change to any member changes the shared SCC hash, and that change propagates to every transitive caller.
- The other 188 transitions did not. Their median ripple was **4.3%**.

Footnote, the earlier call rule (measured, `ripple-any-ident.csv`): the first version counted any identifier that names a function as a call.
- That rule also matched struct fields such as `node->expr`, `node->stmt` and `ty->is_function`, and the parse.c global `builtin_alloca`.
- It inflated the largest SCC to 76 functions.
- It gave a median closure ripple of 25.8% (p90 49.2%) and an amplification of 9.9x.
- The independent verifier, counting only `ident(` as a call, reported SCC 55 and a median of 21.9%. That is close to the 21.6% here, but I did not find the cause of the 0.3-point difference.

## Decision
The median closure ripple is 21.6% (20.9% excluding added functions), above the 10% threshold. **Closure keys over-invalidate.** Whole-closure (Unison-style) addressing is not viable for C here. Per the plan, the next step is keys built from prototypes.

## Caveats
- A call is an identifier followed by `(` that does not come after `->` or `.`. This misses functions used as values, such as function pointers passed as arguments; that drops edges, so it can only lower ripple. A local variable or macro that shares a function's name and is followed by `(` still counts as a call.
- Calls resolve to a static function in the same file first. A name that exists only as statics in other files depends on all of those copies.
- Macros are not expanded, so calls hidden inside macros are missed.
- Identity is by name. A rename shows up as one function added and one removed.
