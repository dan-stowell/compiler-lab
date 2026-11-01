# Research journal

Newest first. "Measured" means a run whose script is in the repo; "estimated" means arithmetic or analysis.

## 2026-11-01: Cycle 4 results so far

**E11, measured executable compression: passes the payload gate.** `experiments/e11-exec-compression-measured/`
- A real file, `payload.bin`, holds chibicc stage2's code in 47,034 B. It uses Re-Pair symbols within blocks, 19-bit target bit-offsets inside jumps, a function entry table, and canonical Huffman operands per slot.
- That is **1.141×** xz -9e(.text+.rela.text) and **1.164×** xz(linked .text), both measured and under the 1.25× gate; native .text is 263 KB.
- It round-trips all 79,400 instructions and decodes from every branch target (2,523), call resume point (2,478) and function entry (324) using the file alone. The verifier wrote an independent decoder sharing no code (`verify/dec.py`) and it agrees.
- Caveat: charging only our side for 1,323 symbol addresses would give 1.27–1.30×.
- Finding: a random-access executable form costs only about 14–16% more than the best general-purpose entropy coder on the same code.

**E12, hot/cold frontier: executable compression doesn't pay as an execution format here.** `experiments/e12-hot-cold-frontier/`
- The C decoder runs at 9.66 ns per guest instruction (measured), so pure decoding is about 70× native.
- With the hottest k functions native and the rest compressed, ≤1.5× native runtime needs **72.4%** of native .text resident (k=86) at the optimistic cost, and 86.9% at 3× cost. Under fairer accounting by the verifier (cold-only payload, narrow tables) it is still 57.1%, above the 50% stop line.
- Why: the profile is skewed (14 functions give 90% of instructions), but the warm tail is still too expensive at about 70× per instruction. Decoder tables (35 KB, reducible to about 13 KB) are as large as the savings.
- Finding: E11's format is a **storage and transfer** format, not an execution format, for a program with chibicc's profile. Demand paging already keeps never-run code out of RAM. A compressed form wins only for code that is warm but rarely run, and here that code is too hot.

## 2026-11-01: Cycle 3 results (both verified)

**E8, slices under clang -O2 (Lua 5.4.6): the slice survives.** `experiments/e8-slices-O2/`
- For 342 extern functions across 32 TUs, the smallest slice that reproduces a function's normalized -O2 asm has a median of **6.46%** of TU tokens (p90 22.6%); excluding system headers, 8.39%. Depth distribution: d=0 for 173, d=1 for 98, d=2 for 38, d=3 or 5 for 8, never for 25. 714 static functions are excluded because their code depends on their callers.
- The first run, lparser.c alone, had N=2 and gave 27%. That was unrepresentative; the full sample reversed it.
- **Mechanism found:** inlining depends on *callers of callees*. clang's last-call bonus inlines a static callee when the slice holds only one of its callers. Adding all callers of included static callees cuts the never-matching targets from 25 to 7.
- Verifier fixes: the comparator ignored string-literal contents (`$.L.str` was parsed as a symbol) and collapsed whitespace inside strings. Both are fixed, with mutation self-tests. Verdicts were unchanged, and the verifier's independent rebuild of 20 targets agreed on d.

**E9, executable compression (chibicc stage2): the fixed-width layout loses; the best variant is undecided.** `experiments/e9-executable-compression/`
- The lower bound for a Re-Pair grammar with fixed-width symbols and operands, a separate operand stream and a label table is 63.9 KB (measured). That is 1.97× xz of the same `.o` text (32,524 B) and 1.58× xz of the linked text. The baseline was corrected in review from clang -Oz to the identical instruction sequence.
- The verifier showed the bound isn't tight. With target offsets in the jumps and Huffman-coded operands per slot (tables counted), the estimate is about 50.2 KB, i.e. **1.22× xz(.text+.rela)**: on the threshold. Being runnable costs most for operands and branch-target addressing.

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
