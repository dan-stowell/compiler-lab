# Phase 2 candidate models (ideator, 2026-11-01)

Prototypes are in `experiments/p0-prototypes/` (single runs, unverified).

- **M1. Grammar-resident data, monoid programs.** Data exists only as a hash-consed DAG (an SLP). A program is (leaf→M, associative ⊗), run as one memoized pass over the unique nodes; there is no decompressor. Prototype `slp.py`: 1001 versions of a 1 MiB file with 10 random 64-B edits each take 1001 MB raw and 2.80 MB as grammar (357×); the fold touches 182,767 nodes. Fails on high entropy, alignment-shifting insertions, and large DFAs.
- **M2. Spacetime memo machine** (generalized HashLife). A program is a local rule; the machine interns (block, level) → centre after 2^(j-1) steps, so the memo table *is* the compiled program. Fails on chaotic rules (rule 30).
- **M3. Seed-addressed memory** (residency as a type). Values are Pinned (bounded by a tier) or Seeded (regenerated from their index). Prototype `gemv.c`: y=Rx with 8192² random R gives 976,557 DRAM fills when R is stored and 1,381 when R is generated inline (707×), at about the same instruction count. Strongest baseline: a structured transform (FJLT). Fails on data with no short generator.
- **M4. Reversible-only programs.** Adjoint without a tape. The win against revolve checkpointing is only c× (<100×).
- **M5. Pull-token (GoI) execution.** Sparse queries of blur^k. The baseline, Halide `compute_inline`, does the same; interning contexts turns it into M2.
- **M6. Linear-sketch (IBLT) state.** Finds differences between snapshots in O(d). A dirty log wins wherever there is a single writer.

Measurement note: use /usr/lib/linux-tools-6.8.0-147/perf. Zen 4 events `ls_any_fills_from_sys.*` and `ic_cache_fill_*` work, but miss L2-prefetcher fills, so streaming baselines are undercounted.

## Round 2 (ideator, 2026-11-01)

Observation: every route to a ≥100× DRAM cut found so far comes from one of three sources: (1) data never needed, because a generator produces it; (2) reuse across versions or queries; (3) the baseline making many passes, one of which can be removed. No new model added a fourth source.

- **R1. Run-once code, never kept.** A description is expanded into machine code in 4 KiB chunks in a single-page RWX ring, each executed once and then overwritten; the expanded program never exists whole. Closest known: JITs and Pike's Blit bitblt compiler (both cache code). Prototype `round2/ring.c` (single runs): 1.42–1.61 ns per generated instruction at 680 instructions per chunk, against 1.16 ns for a native generator loop, so about 1.3× native (vs E12's ~70× decode). Each write-then-execute switch costs ~40–50 ns.
- **R2. Fixpoint-only programs with time skew.** Asynchronous iteration licensed by the semantics. Prototype `round2/skew.py`: PageRank, 99% block-local edges, 7 global passes vs 76 (Gauss–Seidel). Random graphs: 34 passes regardless. **Dead:** the gain is capped by the baseline's pass count (best ≈18×).
- **R3. Huge random objects with global constraints.** A sorted 2^40-element seeded column exists only as a recursive binomial split tree. Closest known: Goldreich–Goldwasser–Nussboim; Sanders' recursive sampling. Open question: do operations compose (joins)?
- **R4. Reuse decided by certificate.** Store the output plus a certificate; a one-pass verifier decides whether the old output holds for a new, opaque input. Ceiling: recompute passes ÷ 1.
- **R5. Lossy residents, exact outputs.** MSB-first bit-planes with interval demand. Ceiling ≤64× by construction.
- Dropped: page-table values, runtime residency probing, dropping dirty lines, bit-sliced version lanes, superposed programs over one scan (shared scans), future-read-time addressing, tier-explicit messages (Sequoia), seed search for real data (fractal compression).
