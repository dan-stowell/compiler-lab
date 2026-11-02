The phase-2 results suggest testing closure under composition, with no assumed advantage over baselines that preserve the same structure. These experiments deliberately end with a **required 4 GiB resident result**: the strongest baseline therefore moves several GB without artificial intermediate materialization. I predict none achieves 100× end-to-end.

For all three, measure the entire chain, including construction, updates, and output. Use pinned, sequential runs, ≤5 perf events, and the brief’s combined DRAM-read counters. Report computed writes separately: a 4 GiB output costs approximately 4 GiB with non-temporal stores or 8 GiB with cold write-allocate stores, including ownership reads and writeback. Drain stores before stopping measurement. Add the VM’s cycle-dependent noise floor; predictions below are not measurements.

**1. Relationships first: weighted complete bipartite relation**

**Representation and operations.** Two arrays, \(L,R\), each containing \(2^{15}\) IDs and bounded integer weights; their Cartesian product denotes \(2^{30}\) edges. Store predicates, per-side projections, and aggregate summaries. Implement factor join, selection, grouped sum, weight update, keyed enrichment, and enumeration.

**Chain.** (1) Join \(L\) and \(R\) into edges; (2) filter even left IDs; (3) group-by left ID, summing right weights; (4) update every right weight by adding three and maintain those sums; (5) join an independently supplied left-ID→bonus table; (6) enumerate surviving edges, emitting one int64 score combining both weights, the aggregate, and bonus. Exactly \(2^{29}\) scores: 4 GiB.

**Baseline and bytes.** FDB-style factorization with maintained aggregates and a fused output traversal is strongest: it preserves the product through all five preceding operations. Both implementations move roughly 1–4 MiB of metadata plus 4 GiB output traffic with streaming stores; approximately 8 GiB with write allocation. Repeated right-side traversal should hit cache. This probably **is factorized representation**, with no new computational model.

**Adversarial tests.**
1. Independent random per-edge weights destroy separability: read 8 GiB, then emit 4 GiB.
2. Random edge overrides grow an exception relation; dense updates approach ordinary storage.
3. Enumeration is the specified sink and establishes the 4 GiB lower bound.
4. Independent left-key enrichment remains compact; joining independent edge membership requires edge-scale work.
5. An unforeseen hash predicate over both endpoint IDs requires \(2^{29}\) predicate evaluations; fusion avoids an intermediate but loses aggregate shortcuts.

**Materialization trigger:** the resident-output sink. A cross-edge predicate forces enumeration earlier, though not necessarily materialization.

**2. Shared latent state: exact rank-two integer matrix**

**Representation and operations.** \(X=UV^\top\), with \(2^{15}\) rows and columns and rank two. Use bounded int64 arithmetic with proven no overflow. Implement projection, row selection, affine updates, contractions, aligned state addition, and aggregate ranking.

**Chain.** (1) Project column IDs and values; (2) retain even rows; (3) add a separable rank-one update; (4) group-by row and sum columns; (5) join another independently generated rank-two matrix on coordinates and add values; (6) top-k rows by updated sums, taking all \(2^{14}\) surviving rows in score order; (7) emit their matrix values: 4 GiB. Ranking determines output order.

**Baseline and bytes.** A fused pipeline over factor tables, using distributivity for row sums, is strongest. Allow it the identical factor metadata and algebra; making it scan a dense matrix would be unfair. Both read approximately 3–8 MiB of factors/summaries and write 4 GiB, assuming blocked traversal. Predict no substantial traffic advantage. This is **exact low-rank algebra**, not a sketch.

**Adversarial tests.**
1. Independent random matrix entries require approximately 8 GiB input and lose low rank.
2. Random cell updates accumulate sparse exceptions, eventually becoming dense.
3. Enumeration requires the specified 4 GiB output.
4. Independent low-rank states add compactly; repeated additions increase rank, while arbitrary coordinate joins can destroy closure.
5. Elementwise cryptographic hashing generally destroys low rank; exact subsequent sums require evaluating every selected cell.

**Materialization trigger:** the output sink; hashing breaks the representation earlier but can still stream.

**3. Intensional results: executable affine sequence**

**Representation and operations.** A small expression DAG describing integer ranges, affine maps, modular filters, aligned joins, block sums, and ordering.

**Chain.** Start with \(2^{31}\) indices. (1) Map \(x=3i+1\); (2) filter even \(i\); (3) join \(y=5i+7\) by index; (4) group into consecutive blocks of 1,024 surviving indices and sum \(x+y\); (5) update each member’s score with its block sum; (6) select the highest-scoring half of blocks; (7) emit member scores: 4 GiB.

**Baseline and bytes.** An algebraically simplified fused pipeline can derive block sums and selected bounds, then generate output directly. Both move negligible metadata plus 4 GiB output. This is **lazy evaluation plus deforestation and algebraic simplification**.

**Adversarial tests.**
1. Replace both affine inputs with independent random int64 arrays: approximately 32 GiB input before selection, unless reusable summaries exist.
2. Random updates require an exception map or dense backing storage.
3. Enumeration imposes 4 GiB writes.
4. Independent permutations of join keys require enumeration and a hash/sort join.
5. Unanticipated global median of hashed scores requires full evaluation and selection passes or external storage.

**Materialization trigger:** the resident sink; the permutation join also needs substantial stored join state in the practical baseline.