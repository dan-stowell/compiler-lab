# Research journal

Newest first. "Measured" means a run whose script is in the repo; "estimated" means arithmetic or analysis.

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
