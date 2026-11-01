# Phase 2 candidate models (ideator, 2026-11-01)

Prototypes are in `experiments/p0-prototypes/` (single runs, unverified).

- **M1. Grammar-resident data, monoid programs.** Data exists only as a hash-consed DAG (an SLP). A program is (leaf→M, associative ⊗), run as one memoized pass over the unique nodes; there is no decompressor. Prototype `slp.py`: 1001 versions of a 1 MiB file with 10 random 64-B edits each take 1001 MB raw and 2.80 MB as grammar (357×); the fold touches 182,767 nodes. Fails on high entropy, alignment-shifting insertions, and large DFAs.
- **M2. Spacetime memo machine** (generalized HashLife). A program is a local rule; the machine interns (block, level) → centre after 2^(j-1) steps, so the memo table *is* the compiled program. Fails on chaotic rules (rule 30).
- **M3. Seed-addressed memory** (residency as a type). Values are Pinned (bounded by a tier) or Seeded (regenerated from their index). Prototype `gemv.c`: y=Rx with 8192² random R gives 976,557 DRAM fills when R is stored and 1,381 when R is generated inline (707×), at about the same instruction count. Strongest baseline: a structured transform (FJLT). Fails on data with no short generator.
- **M4. Reversible-only programs.** Adjoint without a tape. The win against revolve checkpointing is only c× (<100×).
- **M5. Pull-token (GoI) execution.** Sparse queries of blur^k. The baseline, Halide `compute_inline`, does the same; interning contexts turns it into M2.
- **M6. Linear-sketch (IBLT) state.** Finds differences between snapshots in O(d). A dirty log wins wherever there is a single writer.

Measurement note: use /usr/lib/linux-tools-6.8.0-147/perf. Zen 4 events `ls_any_fills_from_sys.*` and `ic_cache_fill_*` work, but miss L2-prefetcher fills, so streaming baselines are undercounted.
