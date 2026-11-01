# Hypotheses (ideator, cycle 1)

Workload: chibicc @ 90d1f7f (8,916 lines, self-hosts in 0.68 s, 316 single-feature commits).

Baseline measurements taken by the ideator (single runs, not yet independently verified):
- source xz -9e 47 KB; stripped binary xz 45.6 KB; source+binary xz together 90.3 KB, against 92.2 KB compressed separately
- parse.c compiles in 79 ms to a 155 KB .o, so recomputing beats fetching below about 16 Mbps
- preprocessed TUs total 584 KB, of which 176 KB are unique lines
- chibicc never frees memory, so peak heap equals total allocation
- stage2 and stage3 objects differ in bytes but disassemble identically

| # | Claim (prediction) | Assumption attacked |
|---|---|---|
| 1 | Source plus resident compiler encodes the binary in ≤50% of xz(binary) | binaries are stored |
| 2 | Regenerating beats a cold read below ~200 KB/s storage | loading is cheaper than making |
| 3 | A single crossover bandwidth per object; ≥90% fewer network bytes at ≤2× CPU | caches fetch outputs |
| 4 | ≥95% of per-function codegen is reusable across consecutive commits | compilation is f(source), each build starts cold |
| 5 | Recompute thunks in place of tokens/AST: ≥10× less peak heap at ≤5× CPU | memory holds values |
| 6 | Hash-consing the AST and types removes ≥40% of allocated bytes | each occurrence owns its node |
| 7 | Spare-cycle peephole search shrinks stage2 text ≥25% | compiler is fixed at ship time |
| 8 | A self-PGO loop gives ≥20% fewer instructions and converges in ≤5 generations | binaries are immutable |
| 9 | A coverage-pruned compiler is ≤50% of the size and still reaches the fixpoint | compilers are general over their language |
| 10 | Re-Pair grammar of the text is ≤40% of raw, ≤5× slowdown when interpreted | an executable is a flat instruction array |
| 11 | Merkle references dirty ≤3% of nodes per commit, with no global symbol table | linking is global |
| 12 | Gossiping memo tables reach ≥90% of a central cache's hit rate | caches need consistent stores |
| 13 | Spot-checking at p=0.05 catches a cheater with ≥99% probability at ≤1.1× CPU | trust needs trusted builders |
| 14 | A cross-compiler zstd dictionary gets chibicc's source to ≤30 KB | programs are compressed independently |
| 15 | Source is recoverable from the binary plus ≤15% side information | source and binary are separate artifacts |
| 16 | A hash-consed, memoizing rewriter recompiles after an edit in ≤2% of the cold rewrites | incrementality needs dependency tracking |
| 17 | Streaming one function at a time keeps the working set in L2; ≥5× fewer LLC misses | the whole TU is resident |

A second, independent list from codex is in `hypotheses-codex.md`, with 15 more. It overlaps on recipes-not-data, replay, gossip and pebbling, and adds search-not-derivation, rescanning instead of symbol tables, IBLT reconciliation, speculative compilation of predicted edits, and approximate analysis with exact verification.
