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
