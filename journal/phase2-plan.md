# Phase 2, cycle 1 plan (revised after review): kill tests first

All three reviews (two adversaries and codex) agreed that, as first planned, P1–P3 were known techniques with weak baselines:
- P1 is compressed pattern matching on SLPs, or monoid-annotated ropes.
- P2 is Gosper's HashLife.
- P3 is Halide scheduling plus rematerialization or pebbling.

Each "100×" was set against a strawman. The measurement plan was also broken: on this VM, `ls_any_fills_from_sys.dram_io_all` reports about 1/40 of streaming traffic, so use `64 × (demand DRAM fills + l2_pf_miss_l2_l3.all)`, cross-checked by computed bytes. The guest's 32 MiB L3 also swallows small inputs.

Before any language is built, each model gets a cheap kill test against its **strongest** baseline. A model survives only if the kill test leaves a ≥100× gap at a named boundary, or shows a property beyond the known technique.

## K1 (P1): Grammar-resident data vs a chunk-summary store
- Strongest baseline: a deduplicating chunk store that keeps, per unique chunk, the DFA transition summary for the query (computed once at query time), and composes summaries per version. This is the same monoid done conventionally.
- The only remaining difference is per-version work: the grammar does O(edits·log n) compositions per version, the chunk store O(n/chunk).
- Workload where per-version work dominates: V = 10^4 versions of a 64 MiB file with 1–10 small edits each (past L3), plus one real history for realism (lua/lua lvm.c, labelled small).
- Output: per-version accept/reject of a regex chosen after storage (|Q| ≤ 64).
- Number: compositions and bytes through the core per query (computed exactly from node and summary counts and sizes), grammar vs chunk store, including summary construction.
- Kill: if the gap is <100× at the workload where it should be largest, P1 is the known rope/finger-tree property and is dropped.

## K2 (P2): Does executing produce a reusable program? (memo-table transfer)
- Claim beyond Gosper: the memo table built while executing on input A works as a compiled program for different inputs B.
- Machine: 1-D HashLife over a radius-1 rule (level-k cells determine their centre for 2^(k-2) steps).
- Test: a cellular computation with structure. Use rule 110 on random finite patches over its standard periodic background, and as a control rule 30.
  1. Build the memo by evolving patch A to 2^24 steps.
  2. Freeze it.
  3. Evolve 20 unseen patches B_i to 2^24 steps, counting lookups served by the frozen table vs new entries.
- Number: the transfer hit rate (frozen hits / all lookups on B) and new entries per B relative to building from scratch.
- Kill test cap: memo ≤ 2^24 entries.
- Rule: transfer hit rate ≥90% with new entries ≤10% of from-scratch means executing produced a reusable program; continue to a structured computation. Otherwise the memo is input-specific and the compile=execute framing adds nothing.

## K3 (P3): Residency on a DAG that cannot be fused
- Workload: a radix-2 butterfly network (FFT shape, integer ops mod 2^32) over n = 2^24 elements, with **seeded leaves** (element i = splitmix(i)), so roots are cheaper to regenerate than to fetch. Output: a 64-bit hash of all outputs.
- Baselines:
  - the best conventional schedule: cache-blocked or recursive (four-step) FFT ordering, where spill traffic is Θ(n log n / log B) per Hong–Kung;
  - full materialization per stage.
- Model: a budget B = 1 MiB (L2). Every value beyond the budget is a recipe recomputed from seeds, with no spilling at all.
- Number: DRAM bytes (corrected counter, plus computed bytes) and instructions, model vs the blocked schedule. Report the recomputation factor.
- Rule: ≥100× fewer DRAM bytes than the blocked schedule means residency-as-meaning moves a known lower bound by exploiting regenerable roots. The cost in instructions is reported, not judged. Otherwise drop P3.
- Note: with seeded leaves, the blocked schedule's compulsory traffic is only spills (no input read). This is the honest comparison.
