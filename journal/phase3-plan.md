# Phase 3 plan: three models, one experiment each

Designs from the ideator (codex's alternatives are recorded in `phase3-designs-codex.md`; they end in a 4 GiB resident sink, so they tie by construction and test nothing). Common rules:
- Input regimes, both reported:
  - (a) **given**: the structure is known at creation (generated or supplied as factors);
  - (b) **discovered**: the input arrives materialized and the model must first build its representation from it. That costs at least one read of the input.
- Baselines get every structural insight a conventional system would have. DuckDB (CLI at /usr/local/bin/duckdb) is the compressed-columnar, fused, vectorized baseline wherever the chain is expressible in SQL. A hand-fused C pipeline is used where it is stronger.
- Measured DRAM: reads via the corrected counter; writes computed and reported separately. Pinned, one measurement at a time. The VM noise floor (~4 MB/Gcycle at an L2 working set) is subtracted using a control.
- Every experiment runs the five adversarial tests and names the operation that forces materialization.
- Hypothesis to test across all three (ideator): an operation composes compactly iff it distributes over the representation's terms. Operations whose value at an element depends on the *interaction* of terms there force materialization: distinct and difference under set semantics, pointwise nonlinearity under rank, composition of unaligned index maps under piecewise models. The cost then is the number of overlap cells.

## Q1: Relationships first (rectangle-cover relations)
- Representation: a relation R = ∪ᵢ Aᵢ×Bᵢ (bitset sides). Operations: select (intersect a side), compose (A×B)∘(C×D) = A×D (weight |B∩C| under bag semantics), union, projection.
- Data: n = 2^17 nodes, K = 32 overlapping communities of 4096×4096 (≈537M edges, 4.3 GB as u32 pairs). P(c, item) is 32 rectangles over 2^16 items.
- Chain (≥5 ops): select a < n/2 → R∘R → ∪ R (paths of length ≤2) → ∘P → **set-semantics** projection: the distinct (a, item) pairs reachable, then count per item → top-k items.
  - Also run the bag-semantics version (path counts), where d-representations tie.
- Baselines: DuckDB on the edge list (join, DISTINCT, GROUP BY); and a fused C aggregate over CSR (SpMV-style) for the bag version.
- Adversarial:
  1. random edges (Boolean rank ≈ |R|);
  2. delete 1% of edges;
  3. enumerate the distinct (a, c) pairs;
  4. compose two independently generated covers;
  5. "distance exactly 2" (R∘R − R).

## Q2: Shared latent state (exact low-rank collection)
- Representation: X = U·Vᵀ, N = 2^20 rows, M = 1024 columns, r = 8 (4 GB as float32). Use exact integer arithmetic or document float tolerance.
- Chain (≥5 ops, ≥2 pipeline breakers): scale columns → select a column subset → centre → covariance → top-k principal directions by power iteration (≥10 passes for a materialized baseline) → project all rows onto them and return the top-100 rows by score.
- Also: a sequence of **20 independent queries** over the same implicit state (different column subsets and weights), since the phase hypothesis is about preserving structure across operations.
- Regimes:
  - (a) U regenerated from a seed;
  - (b) X given materialized, so the model must first fit or recover its factors in one pass (an exact rank-r recovery via sampled rows/columns or randomized range finding, with error reported).
- Baselines: a hand-fused C pass over materialized X per pipeline breaker; NumPy/BLAS for the covariance. DuckDB isn't suited.
- Adversarial:
  1. full-rank random X;
  2. 1% sparse cell edits (low-rank plus sparse);
  3. enumerate all cells;
  4. add two independent states (rank r₁+r₂), then join through an independent permutation;
  5. ReLU applied elementwise, then covariance.

## Q3: Intensional results (piecewise-polynomial columns)
- Representation: a column is P pieces of degree-≤d polynomials over index intervals. Operations:
  - zip-mul (degree adds);
  - prefix-sum (degree +1);
  - filter by a polynomial predicate (root splitting plus a piecewise-affine index remap);
  - sort as a merge of monotone pieces, with rank and quantile queries in O(P log N).
- Data: 2^30 rows; price and qty piecewise-linear, P = 10^4.
- Chain (≥5 ops): price·qty → running total → filter total ∈ [lo, hi] → lag difference → sort → median and deciles.
- Baselines: DuckDB over materialized columns (it compresses and fuses; it must sort or use quantile aggregates); a hand-fused C pipeline plus radix sort.
- Adversarial:
  1. random values (P ≈ N);
  2. 10^5 point updates;
  3. enumerate the sorted output;
  4. zip two independently pieced columns;
  5. sorted[i] − original[i] (composing unaligned index maps).

## Success criterion
At least one chain with ≥100× less measured DRAM than the strongest baseline, end to end, in regime (a), and ideally (b). For each model, the precise operation that forces materialization, with a measured confirmation.


## Revision after review (adversary and codex, 2026-11-02)

Both reviews converge:
1. **Regime (a) restates phase 2's source 1.** A baseline handed the same seed or factors and fused with the generator also moves ~0 DRAM.
2. **Regime (b) caps Q2 and Q3 at ≤2×.** The strongest baselines need 1–2 passes (XᵀX plus a sketch; histogram plus multiselect), and discovery costs ≥1 pass.
3. **Given equal structural information, a structure-aware conventional system matches each model wherever operations distribute.** Cover-aware bitset/factorized joins, factor-aware BLAS, and pieces plus selection all qualify.
4. Q1 as specified was degenerate: overlaps of ~128 make every pair non-empty.
5. Q3's chain collapsed: a non-negative product makes the running total monotone.

Revised design, the same for each model:
- **Three implementations:**
  - (i) a conventional engine fed the same base data, which expands and materializes intermediates as such engines do: DuckDB, or a fused C pipeline;
  - (ii) the strongest *structure-aware* conventional baseline, given the same compact input;
  - (iii) the model.
  - Report DRAM (measured reads, computed writes) for all three over the whole chain. Ratio = baseline ÷ max(model, floor × Gcycles).
- **Representation-size trajectory:** after each operation of the chain and each adversarial operation, record the size of the compact state (rectangles / rank / pieces / overlap cells). The answer to the critical question is the first operation where it grows to Θ(output) or Θ(N), with the ideator's distributivity criterion as the prediction to test.
- **Adversarial tests:**
  - 1 (incompressible input) and 3 (enumeration) are stated as one line of size arithmetic each;
  - 2, 4 and 5 are run as size counts;
  - DRAM is measured only for the headline chain and the operation that forces expansion.
- **Q1:** side sizes cut so expected overlaps are ≲1. Bag/set semantics fixed (a disjoint cover or an explicit multiset). Baseline (ii) is row-deduplicated adjacency (WebGraph-style) with Boolean bitset products. Baseline (i) is DuckDB only where it can finish; otherwise report its tuple counts.
- **Q2:** baseline (ii) is factor-aware linear algebra. Baseline (i) is a 1-pass XᵀX plus range sketch over materialized X. The point is the op where factor algebra breaks (ReLU, independent permutation join); measure the cost of that op.
- **Q3:** signed product. Quantiles by exact selection (DuckDB `quantile_disc`, plus a C 2-pass histogram/multiselect), not a full sort. Baseline (ii) is pieces-aware selection. Exact algorithm and arithmetic assumptions are stated.
- **Measurement hygiene:** all perf DRAM runs take a shared lock (`flock /tmp/claude-1000/-home-exedev/8dacbf12-f718-44f5-a239-aefe4726da86/scratchpad/perf.lock`), pinned to core 2. Report the noise-floor control alongside.
