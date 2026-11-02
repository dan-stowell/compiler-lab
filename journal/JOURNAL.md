# Research journal

Newest first. "Measured" means a run whose script is in the repo; "estimated" means arithmetic or analysis.

## 2026-11-02: Phase 2, kill tests K1–K6 (all verified)

12 candidate models were generated first (`phase2-models-*.md`). Plan review found that the first three picks were known techniques against strawman baselines, so each got a kill test against its **strongest** baseline. Known techniques that production compilers don't use are recorded as such, per Dan.

| | Model | Strongest baseline | Result |
|---|---|---|---|
| K1 | data only as a hash-consed grammar; programs are monoid folds | dedup chunk store with per-chunk DFA summaries | 6.3× ops at 64 MiB × 10^4 versions; chunks win on real lvm.c history; a tree manifest *is* the grammar (≈1×). Gap grows √n/log n (100× at ~32 GiB). Dead; known (Merkle/rope). |
| K2 | executing builds the program (HashLife memo as compiled artifact) | building from scratch | Rule 110: frozen memo leaves 82% new entries for unseen inputs; 42% even from 1000 training patches (3.2M entries). Input-specific. Dead. |
| K3 | residency as meaning: L2 arena, recompute instead of spill, seeded leaves | blocked four-step butterfly (one 64 MiB spill) | 75 MB vs noise-floor 15 MB (4.9×) at 28× recompute, 47× instructions. Known Savage pebbling tradeoff; 100× unmeasurable on this VM. |
| K4 | run-once code: data baked into throwaway machine code in a small ring | 8-lane table DFA, 1.62 cycles/B | 2.35× slower, but within 3× ("viable"). Execution ≈1.15× the table; generation ≈2 cycles/B is the gap, bounded by code-write bandwidth (~15 code B per input B). |
| K5 | huge random objects as seeded binomial split trees | materialization, impossible at 2^40 | Range count ~2·log₂N (68 visits at 2^40). Exact joins Θ(N) (0.0917 visits/row); fundamental for independent generators. |
| K6 | run-once code for op streams executed once | best of computed-goto, predicated and hybrid interpreters | Synthetic random opcodes 3.9×; natural stream (Lua PCs executed once, 86 real modules) **1.02×**; predictable control 0.29×. Break-even 0.066 mispredicts/op, 2× at 0.29. |

**Principles found**
1. **Three sources.** Every route to a ≥100× DRAM cut found reduces to: data never needed (a generator produces it), reuse across versions or queries, or a removable pass. No model added a fourth source.
2. **D vs S.** A code generator sees the data it reads (D), never the run-time state (S). Run-once code removes work only when decisions depending on D alone are expensive for a data-generic loop. Immediates never save bytes for once-used data.
3. **Generated objects answer only what their generator generates top-down.** Counts are O(log N); sums and joins of independent objects need every leaf.
4. **Run-once code must be straight-line.** Calls from fresh code to handlers miss the BTB (~0.67 re-steers/op, ~20 cycles each).
5. **Measurement on this VM:** `ls_any_fills_from_sys.dram_io_all` sees ~1/40 of streaming reads; add `l2_pf_miss_l2_l3.all`. The 6th perf event reads 0. Sibling-vCPU noise sets a ~4–15 MB DRAM floor per Gcycle. Compare cycles, not wall time.

## 2026-11-01: Phase 2 begins; cycle 7 results (E15, E16)

**E15, output churn of E13 units (ceiling).** `experiments/e15-output-churn/`
- Over 477,399 function-transitions in 1500 lua/lua commits, a unit's whole -O2 output changes at rate 0.0147. The slice key changes at 0.079 (5.4× as often), closure bodies at 0.0157, target bodies at 0.0075.
- 36% of output changes are context-only.
- Also found that E14's population was inflated (glibc extern-inline functions counted as targets).

**E16, layout-projection key: a compile-free key built from what a function *uses*, with no false hits observed.** `experiments/e16-projection-key/`
- The key is a union over the target and its reached statics: bodies, desugared expression types, accessed fields' bit offsets and widths, record size and alignment, whole layout for by-value use or bitfields, clang's own sizeof/alignof for every type mentioned, enum constants, callee prototypes with attributes, and globals' types and const initializers.
- Extraction is a syntax-only clang pass, about 108 ms per unit.
- Results: **22,771 informative hits** (slice key changed, projection equal) in 203 commits, **0 false hits**. Per-hit 95% bound 0.013% (below E10's 0.035% effect); commit-clustered bound 1.48%. The key changes 2.12× as often as the output, and 2.5× less often than the slice key.
- Over-invalidation comes mostly from containing-record size (`global_State`, `GCUnion`).
- **Adversarial verification found three rounds of holes, each fixed with mutation tests (19/19):**
  1. typedef expansion: 19 false hits in the first run;
  2. typedef alignment attributes and packed-enum sizeof: hand-built, absent from history;
  3. bitfield access unit, where widening a neighbour turns a `movw` store into `movl`: hand-built, absent from history.
- Finding: a function's dependencies can be stated as *facts it uses*, finer than declarations, and such a key behaved soundly on 1500 commits of real history. But a sound projection must model the **compiler's lowering decisions** (store widths, alignment, ABI), not just C semantics. Every hole was a lowering fact.

Phase 2 (inventing computational models) started; see `phase2-brief.md`, `phase2-models-*.md`, `phase2-plan.md`. Kill tests K1–K5 are below as they land.

## 2026-11-01: E10 (verified) and E14 (settled from existing data)

**E10, hidden dependencies of learned slices: unsound for bit-identity, at a low rate.** `experiments/e10-hidden-deps/`
- Over the newest 1500 lua/lua commits, slices learned at commit N (depth frozen) and reused at N+1 give n=63,504 counted hits, i.e. TUs whose token stream changed; the hit rate is 60.6%.
- **22 (0.035%)** of these have full-TU -O2 asm that changed while the slice didn't. They fall in 12 commits; the commit-clustered 95% interval is 0.011–0.070%.
- Cause, in every case: a same-TU callee outside the slice became inlined. Usually its body changed; at least once the effect was two levels deep (`luaH_newkey` → `luaH_finishset` → `luaH_set`).
- The verifier reproduced four cases by hand and the 200-commit pilot.
- Read with E13: these are differences from the whole-TU compile, not wrong code. A cache keyed on exactly what the worker compiles is sound by construction. A cache that promises whole-TU output is not.

**E14, key lifetime: stop (keys churn too fast for spare-cycle investment).** Computed by the plan reviewer from E10's data in about a minute (measured); being packaged as `experiments/e14-key-lifetime/`.
- Per-transition change rate (mean lifetime ratio vs per-TU):
  - per-TU hash: 0.183 (1.0×)
  - E14 TU-free key, all header declarations included: 0.142 (**1.29×**)
  - E8-minimal slice: 0.051 (3.6×)
  - body hash, the ceiling: 0.0049 (37×)
- In 81% of changed-TU transitions, every E14 key in the TU changes at once. The over-inclusive header context is what churns.
- Finding: a TU-free key that is sound by construction lives barely longer than a TU hash. Only the minimal slice key gets a few times longer, and it is under the 5× bar. Lifetime is also necessary but not sufficient: no optimization-benefit estimate exists yet. Caching expensive per-function optimization doesn't pay on this workload's churn.

## 2026-11-01: Cycle 5: dropping bit-identity

The ideator pointed out the assumption shared by every experiment so far: "correct" meant bit-identical to the whole-TU compile, which forces mechanisms to copy compiler quirks. Its spot-checks (single runs): per-TU flag search beats -Oz by only 1.0%; zstd dictionaries trained on other programs shrink Lua's .text by ≤1.1%; across Lua 5.4.6→5.4.7, 572/644 functions are unchanged after normalization.

**E13, TU-free build of Lua: viable at no measurable instruction cost (verified).** `experiments/e13-tu-free-build/`
- Lua's 19 core files were split into **331 objects, one per extern function**. Each object compiles its file with every other extern body replaced by a prototype; median slice 1.57% of file tokens. Statics are copied per object (96 extra copies, function bytes 1.148×).
- Address-taken statics reached by several targets (`boxgc`, `dothecall`) became single hidden globals. The safety check caught the plan's wrong assumption ("none exist"): 17 address-taken statics.
- The official test suite passes ("final OK") and benchmark outputs are identical. Instructions retired: geomean **1.0008×** (interval 0.978–1.025 over 5 benchmarks; per-benchmark 0.98–1.024). Cycles: 1.061, inconclusive (relinking in a shuffled order alone moves cycles by 4.9%).
- Five helpers no longer inline into `luaV_execute` (idiv, mod, modf, shiftl, tointegerns) with no instruction-count effect.
- Caveat: extern const data is duplicated as local copies (`lua_ident`, `luaT_typenames_`); nothing observable was found.
- Finding: for this program, cross-function knowledge inside a TU is not load-bearing. A function plus the statics it reaches is a sufficient compilation unit. With E8 (tiny slices) and E7 (stable outputs), that makes per-function, content-addressed, TU-free compilation practical, **provided bit-identity with the whole-TU compile isn't required**.

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
