# Research journal

Newest first. "Measured" means a run whose script is in the repo; "estimated" means arithmetic or analysis.

## 2026-11-01: Architecture challenge, then cycle 3

An adversary challenged the "slice-addressed per-function compilation" direction suggested by E5 and E7. Its points:
- Most of it exists already: Bazel/RBE, distcc pump mode, rustc red-green and early cutoff, Zig per-Decl incrementality, ThinLTO summaries.
- The slicer is itself dependency analysis, recomputed on every build, so it isn't free.
- The first E8 design (a slice memo over history) would pass by construction.

The only possibly new claim is that a small, fixed amount of callee information is enough. Under chibicc's non-optimizing codegen, that is a property of the compiler. Cycle 3 therefore tests whether slices survive clang -O2 (`cycle3-plan.md`). Plan review added:
- A static function's code also depends on its callers (constant propagation, dead-argument elimination), so only extern functions are targets.
- No body-less declaration can stand in for "defined here" under clang.

## 2026-11-01: Cycle 2 results (both verified)

**E5, dependency slices: the rule fails as written, but one bit fixes it.** `experiments/e5-dependency-slices/`
- Strict slices (callees as prototypes) reproduce chibicc's asm for only 23/116 parse.c functions. All 93 failures have one cause: codegen.c:143 emits `lea f(%rip)` for callees defined in the TU and `mov f@GOTPCREL` for callees only declared.
- With callees as empty stubs (carrying the "defined here" bit), 116/116 verify. The median slice is 2.85% of TU tokens (p90 4.14%, max 8.39% for `stmt`); context alone is 2.5%.
- The verifier's own greedy-minimized slices were never larger, and a stricter comparison that also checks string-literal data still passes 116/116.
- Finding: the header-struct cluster does not drag in the TU. What a prototype lacks is one bit.

**E7, output-addressed asm over history: both rules hold.** `experiments/e7-output-addressed/`
- HEAD chibicc compiled all 315 historical transitions (1034 compiles, 30 s CPU).
- The per-function asm hash changes for a median 26.4% of functions raw (p90 60.3%), but 2.1% (p90 16.9%) after stripping `.loc` and renumbering labels per function. That is close to the 1.5% body rate and far below E2's 21.6% closure keys.
- The file-wide label counter and `.loc` line numbers each account for 7–17 points, depending on which is removed first; both are needed.
- Verifier fixes: hash visibility lines (static↔extern changes were missed), and correct the order-dependent decomposition. Caveat: named static data is in no function's hash.
- Finding: raw output ripple comes from the compiler's global state, not the program. Output addressing works once that state is removed.

## 2026-11-01: Cycle 2 setup

Two of the five candidate experiments were settled on paper by plan review, without being built:
- **E4, ring heap (dead, analytic).** The idea: allocate from a ring and overwrite the oldest bytes, with no liveness tracking. main.c lexes parse.c first, and preprocessing passes ordinary tokens through uncopied. Codegen reads `node->tok->line_no` and `->file` for every `.loc` at the very end (codegen.c:693, 1189). So the oldest allocations are live until exit, and S_min ≥ 39% of bytes even from E1's phase totals (estimated at ≈90%), against a 10% rule. Also, an LD_PRELOAD shim cannot see loads, so a checked ring can't be built that way; it needs an access trace. Lesson: in a batch pipeline, liveness follows source position, not allocation order.
- **E6, lex once across TUs (passes, uninformative).** Each header is lexed once per TU, so `chibicc -M` across the 9 TUs gives 4.60 MB lexed against 0.69 MB distinct (6.6× by bytes, estimated). That passes a 3× rule regardless, and it is the standard case for precompiled headers. Dropped as unsurprising.

Running: E5 (dependency slices) and E7 (output-addressed asm over history). See `cycle2-plan.md`.

## 2026-11-01: Cycle 1 results (3 of 3 predictions failed; all independently verified)

**E1, token regeneration gate: dead.** `experiments/e1-token-gate/`
- In cc1 compiling parse.c, 46.1 MB is allocated in total (measured). Tokens are 17.05 MB (37%). Tokens still reachable from the program after parse are 1.76 MB (3.8%), under the 25% bar.
- 90% of token bytes are garbage that is never freed. Ordinary free() would reclaim about 34% of bytes; regeneration could add only about 4%.
- The verifier reproduced this with gdb hit counts, valgrind massif (peak 28.27 MB), and an independent walk of every pointer field found through debug info.
- Finding: memory in a batch compiler goes to dead values, not live ones, so regenerating values attacks the wrong term.

**E2, Merkle closure ripple over 316 commits: over-invalidates.** `experiments/e2-merkle-ripple/`
- Body hashes change for a median 1.5% of functions per commit. Closure keys (body plus callee keys, with SCCs as units) change for a median 21.6% (p90 40.9%), against a ≤10% rule.
- The largest SCC is 55 functions: parse.c's recursive descent. Commits that touch it ripple a median 31.6%; the others ripple 4.3%.
- Bug found by the verifier: matching on identifiers alone counted `node->expr` as a call to `expr()`, which inflated the SCC to 76 and the median to 25.8%. Fixed so that only `ident(` counts as a call, and the fix was re-verified.
- Finding: Unison-style identity on C transitive closure is dominated by one recursive-descent SCC. Identity has to cut at interfaces (prototypes), not at bodies.

**E3, search instead of derivation (4-bit stack VM): not at this scale.** `experiments/e3-search-not-derive/`
- Search over distinct behaviours (BFS, deduplicated on exhaustive 4096-input value vectors) finds programs averaging 0.768 of naive postfix length, against a ≤0.75 rule. Search was shorter on 83/200, with 0 timeouts, 177 s CPU and 104 MiB peak.
- Against a postfix compiler with constant folding and simple identities (verifier's baseline), the ratio is **0.917**, and search is shorter on only 34/200.
- The verifier checked all programs exhaustively with an independent VM and brute-forced optimality on a sample (`verify/`).
- Finding: almost all of the gain is degenerate-subterm simplification. Only one found program used stack sharing (DUP/OVER). Search cost grows about 50× per two instructions.

**Cross-cutting:** all three mechanisms lost to a simple conventional baseline: free(), keys built from prototypes, and constant folding. The next cycle should pick questions where no cheap conventional answer exists.
