# K5 results: do huge random objects compose?

Plan: `journal/phase2-plan.md`, K5. `run.sh` produces everything under **Measured** and **Estimated** (`results/`, about 10 min on 8 cores).
Each line says whether it is measured (full computation) or estimated (sampled, unbiased, with standard error).

## Model (`k5.c`)
- A column is (N, [lo,hi), seed): the sorted multiset of N iid uniform 64-bit keys in [lo,hi). Value = key.
  It exists only as a split tree. A node ([a,b), n) splits at m = a + (b−a)/2, and the left count is Binomial(n, (m−a)/(b−a)).
  The draw comes from a counter RNG seeded by hash(seed, a, b), and the node's range identifies its path.
  A node with n ≤ 64 (or width 1) is a leaf: n keys iid uniform in [a,b) (Lemire's unbiased bounded draw), insertion-sorted.
  Recursive binomial splitting plus iid uniform leaves is exactly the law of N sorted iid uniforms.
- Binomial sampling uses sequential pmf inversion when n·p < 10 and Hörmann's BTRS (an exact transformed-rejection method) otherwise.
  The BTRS acceptance test uses Stirling tail corrections and log1p/fma residuals, so it carries no n·ε error at n = 2^40.
  It is exact up to double rounding. For non-power-of-two widths, p itself is rounded to a double.
- (a) Range count/sum: descend, and take a fully covered node's count without descending further.
- (b) Equi-join count Σ_k cntA(k)·cntB(k) between two independent columns (different seeds) by simultaneous descent:
  - prune when either count is 0 or the ranges are disjoint;
  - split both nodes when their ranges are aligned, otherwise split the wider one;
  - at a leaf, probe its sorted keys down the other tree, partitioning them at each midpoint, and sort-merge against the other tree's leaves.
- A visit is one node expansion (one binomial draw) or one leaf materialization.

## Measured: correctness (`results/verify.txt`, `results/binomtest.txt`)
- All 8 verify runs PASS: N = 2^20 and 2^26, each with W = 2^64, W = 4N, W = 4N with B shifted by 1.5N (partial overlap, unaligned trees), and N+3 over W = 4N−1 with shift 12345 (odd sizes).
  Each run checks the following:
  - in-order materialization gives exactly N values, sorted, in range;
  - a reverse traversal followed by an independent radix sort gives the identical array;
  - a χ² over 1024 buckets is consistent with uniform (|z| ≤ 1.86 in every run);
  - 20 random range counts and sums match the materialized column;
  - the tree join count equals the materialize+sort+merge join count. At 2^26: 0 (W = 2^64), 16,782,326 (W = 4N), 10,489,960 (overlap).
- Binomial sampler, 10^6 draws: mean z within ±2 and variance ratio within 0.0015 of 1, at n = 20, 1000, 2^20 and 2^40.
  A χ² against the exact pmf at n = 1000 gives z = −0.39.

## Measured and estimated: (b) join visits vs N (`results/join.txt`, `results/fit.txt`)
- Measured covers N = 2^16 to 2^30 (full descent). Estimated covers N = 2^30 to 2^40:
  - exact top 2^(log N − 22) aligned node pairs, plus 64 depth-k subtree pairs sampled without replacement and joined fully, scaled up;
  - relative standard error is ≤ 0.1%;
  - at N = 2^30 the estimate is within 0.01% of the measured value in all three key spaces.

| N | W = 4N (dense) visits | join count | W = 1024N visits | join count | W = 2^64 visits | join count |
|---|---:|---:|---:|---:|---:|---:|
| 2^16 measured | 6.03e3 | 16,419 | 6.03e3 | 70 | 5.96e3 | 0 |
| 2^20 measured | 9.61e4 | 262,491 | 9.61e4 | 1,004 | 9.62e4 | 0 |
| 2^26 measured | 6.15e6 | 16,782,326 | 6.15e6 | 65,814 | 6.15e6 | 0 |
| 2^30 measured | 9.84e7 | 268,419,199 | 9.84e7 | 1,047,486 | 9.84e7 | 0 |
| 2^34 estimated | 1.57e9 | ≈4.29e9 | 1.58e9 | ≈1.68e7 | 1.57e9 | ≈0 |
| 2^40 estimated | 1.01e11 | ≈2.75e11 | 1.01e11 | ≈1.07e9 | 1.01e11 | ≈7.8e4 (expected 6.6e4) |

- In every key space, visits/N = 0.0917 at every N. Fitted exponent of visits vs N:
  - 0.9999 (dense), 0.9999 (1024N) and 1.0005 (2^64), measured 2^16..2^30;
  - 1.0000, 1.0000 and 1.0002, measured + estimated 2^16..2^40.
- Join size does not matter. The almost-empty 2^64 join costs the same as the dense one.
  Each node pair is a fresh independent sample, and a zero-count prune can only fire below the leaf scale (N/2^d < 64 per node), where the trees already stop.
  So the descent expands essentially every internal node of both trees and materializes every leaf: 2N keys generated, the same generation work as materializing both columns.
- At N = 2^26 the tree join took 3.5 s, against 0.54 s for the merge step alone over pre-materialized arrays.
  Materialization plus sort costs more than that and needs 16N bytes, about 1 GiB here.

## Measured: (a) range count / sum (`results/range.txt`, W = 4N, 16 random ranges)
- Range count: mean visits are 20.3 at 2^16, 29.2 at 2^20, 48.9 at 2^30 and 68.5 at 2^40, about 2·log2 N. That is O(log N), as expected.
- Range sum, with values = keys: 815 visits at 2^16, 14,783 at 2^20 and 18,198,239 at 2^30, which is Θ(N).
  A node's count is generated top-down, but its key sum is not, so every fully covered subtree must be expanded to its leaves.
  Cheap aggregates need the aggregate itself to be split top-down (count yes, sum no, as defined here).

## Decision
- Rule: sub-linear join visits mean the operations compose; Θ(N) means the model supports only single-object queries.
- **Measured exponent 1.000 in sparse, medium and dense key spaces, up to 2^30 measured and 2^40 estimated: Θ(N). Record that this model supports only single-object queries.**
  Range count is O(log N). A join between two independent generated columns costs the same as generating both, at any output density.
- What it does buy (not a rule criterion): the join streams with O(log N) memory and moves no column bytes.
  The sort-merge baseline must hold 16N bytes, impossible at 2^40 (16 TiB).
  It trades data movement for full regeneration, which fits the brief's "bytes, not speed" criterion. The asymptotic work is not reduced.
- **Estimated / interpretation:** sub-linear joins would need correlated columns, so that structure is shared between the generators (for example, B derived from A's tree).
  Independent seeds leave nothing to prune.
