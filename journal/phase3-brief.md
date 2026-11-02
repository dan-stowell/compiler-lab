# Phase 3 brief: computation over implicit worlds (2026-11-02)

Hypothesis (Dan): substantial I/O reductions require preserving structure *across* computations, not just regenerating individual values. Phase 2 established the limits: K5 (independent generators make joins Θ(N)), K1 (a Merkle tree is the chunk store), K4/K6 (run-once code is bounded by generation bandwidth). Don't repeat them with new representations.

Three competing models, each built minimally (no framework):
1. **Relationships first:** store or generate relationships; objects are projections.
2. **Shared latent state:** large collections are observations of a compact underlying computational state.
3. **Intensional results:** operations return executable descriptions of results; later operations compose over those descriptions.

For each model:
- A chain of **≥5 distinct operations** where a conventional implementation repeatedly materializes substantial intermediate data.
- Strong baselines: factorized databases (f-representations / FDB-style), compressed columnar execution (dictionary/RLE plus late materialization), lazy iterators, and fused query pipelines. Use the strongest that applies.
- **Total bytes moved across the whole chain**, including any eventual materialization, measured as DRAM traffic on hardware counters (reads: `64×(ls_any_fills_from_sys.dram_io_all + l2_pf_miss_l2_l3.all)`; writes are undercounted and must also be computed), plus computed bytes.
- Adversarial tests, all required:
  1. independent, incompressible inputs;
  2. updates that disrupt correlations;
  3. queries that require enumeration;
  4. composition of independently generated states;
  5. an operation the original generator did not anticipate.
- Target: at least one workload with ≥100× less **measured** DRAM traffic than the strongest baseline, not just smaller serialized output.

Critical question: can a model make useful operations compose while the information stays compact? If not, name the precise operation that forces materialization, and why.

Measurement constraints on this VM (phase 2):
- Sibling-vCPU noise gives a DRAM read floor of ~4 MB per Gcycle at an L2-sized working set.
- So a 100× claim needs the baseline to move ≥ ~100× the model's floor: baselines of several GB, and models that stay within a few Gcycles.
- Pin runs; compare cycles, not wall time; ≤5 perf events per run; run DRAM measurements one at a time.
