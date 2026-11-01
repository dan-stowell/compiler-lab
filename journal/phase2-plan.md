# Phase 2, cycle 1 plan: three computational models

Chosen from 12 candidates (`phase2-models-*.md`). Each challenges a different assumption; each gets a minimal language, one operation, one number at a named boundary, a failure workload, and a scaling question. Data movement is measured with perf Zen 4 fill counters plus cachegrind, with computed byte counts as cross-checks; instructions and generated code bytes are counted too.

## P1: Grammar-resident data (assumption: data must be materialized to be computed on)
- Language: data is a hash-consed SLP over 64-byte leaves (balanced, persistent; edits create O(log n) nodes). A program is `fold(leaf: bytes→M, combine: M×M→M)` with `combine` associative. Evaluation is a memoized pass over the unique nodes; nothing is ever expanded.
- Operation: run a regex (chosen **after** the data is stored, so nothing can be precomputed) over every version of a file history. M is the DFA's transition function (|Q| states → array of |Q| bytes); combine is function composition.
- Input: real history, the lua/lua `lvm.c` across all of its commits (use content-defined chunking so insertions don't shift every leaf; record the chunker). Synthetic random edits only as a secondary.
- Baseline: each version stored zstd `--long` compressed, stream-decompressed into an L1-sized buffer and scanned by a table DFA (the same regex).
- Number: bytes moved from DRAM (fills × 64) plus bytes through the core (loads), baseline vs model, for the whole query over all versions. Include grammar construction separately (it is a one-time storage cost).
- Rule: ≥100× fewer DRAM bytes **and** ≥10× fewer bytes through the core means the model wins at that boundary. Report the DFA size where it stops winning (failure: |Q| growth) and on random (high-entropy) data.

## P2: Spacetime memo machine (assumption: compiling and executing are separate; a program is stored code)
- Language: a 1-D cellular rule of radius 1 (an 8-entry table). The machine is HashLife over 1-D macrocells: the memo table maps (macrocell) → centre after 2^(k-1) steps, and nothing else exists.
- Kill test first (about 1 hour): for rule 110 on a random 1K-cell patch over its periodic background, count unique macrocells per level and memo entries to reach 2^30 steps. Compare with rule 30.
- Operation: compute the state of a 2^20-cell window after T=2^40 steps.
- Baseline: bit-sliced AVX2 direct stepping (bytes = n/8·T; estimated, since it can't be run to 2^40). Measure the direct baseline at T=2^20 and extrapolate linearly (labelled).
- Number: memo bytes + DRAM traffic for the query vs baseline bytes at the same T. Find T where HashLife overtakes.
- Failure: rule 30 (chaotic). Scaling question: does any useful computation (e.g. a cellular adder or sorter on structured input) have this redundancy?

## P3: Residency as meaning (assumption: memory holds whatever the program computed; residency is an implementation detail)
- Language: a straight-line DAG language. Every node is declared `pin` (counts against a static byte budget B checked before running) or `recipe` (recomputed from its inputs at each use, never stored). A program whose pinned live set can exceed B is rejected. The evaluator executes within a B-byte arena.
- Operation: a deep, narrow-output dependency graph, e.g. a 2^20-element pipeline of 32 stencil stages with one scalar output. Under B = 32 KB (L1) the checker forces most stages to be recipes.
- Baseline: the same computation materializing each stage (B unbounded), and an optimized checkpointing (pebbling) schedule at the same B.
- Number: DRAM bytes and total instructions, model vs both baselines, plus the recomputation factor.
- Rule: ≥100× fewer DRAM bytes than materializing, at any slowdown, together with a measured position against pebbling, means residency-as-meaning is worth a scaling study. Failure: high-fan-out DAGs (recomputation grows exponentially with depth); measure where the instruction count explodes.
