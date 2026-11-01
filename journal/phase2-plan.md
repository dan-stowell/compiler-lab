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

## K4 (R1): Run-once generated code on a data-dependent program
- Semantics: a program is a generator emitting x86 machine code in chunks into a fixed 4 KiB–64 KiB RWX ring. Each chunk is executed once and overwritten. Generated code never exists whole, and no code bytes are fetched from DRAM.
- Operation: run a DFA (|Q| = 256, from a fixed regex set) over a 1 GiB input, where the generator **specializes code to the data**: for each 4 KiB input block it emits straight-line code with the input bytes as immediates and transitions resolved at generation time. The generated code is the input fused with the program.
- Baselines: a table-driven DFA (the strongest; its data movement is the input once plus the table) and an interpreter.
- Number: total cycles, instructions, `ic_cache_fill_*`, and DRAM bytes (corrected counter), model vs table-driven, including generation.
- Rule (judged on what the model reveals, not speed): report the generation cost per input byte and per generated instruction against R1's 1.3× microbenchmark. If the data-specialized run-once code is within 3× of the table-driven DFA in cycles, "fusing data into throwaway code" is a viable execution model, so look for a workload where specialization removes work the table version must do. Above 10×, the model only works when generation is data-independent.
- Failure workload: tiny chunks (<64 instructions), measured explicitly.

## K5 (R3): Do huge random objects compose?
- Semantics: a sorted column of N seeded values (N up to 2^40) exists only as a recursive split tree. Node (range, count, seed) splits its count binomially between halves using its seed. Leaves materialize at ≤64 values.
- Operations: (a) range count and range sum in O(log N) node visits, the expected easy case; (b) an equi-join count between two independent columns with overlapping key ranges, by simultaneous recursive descent.
- Baseline: materialization, which is impossible at 2^40 (stated), and at N = 2^26 for correctness checks against an exact sort-merge join.
- Number: node visits as a function of N for (b).
- Rule: if join visits grow sub-linearly (for example O(√N) or polylog for a sampled-key-space join), operations compose, and generated objects support relational algebra without materialization. If they grow Θ(N), the model supports only single-object queries; record that.

## K6 (follows K4): Run-once code for op streams executed once
- Principle (ideator): the generator sees the data D it reads, never the run-time state S. Run-once code removes work only when decisions depending on D alone are expensive for a data-generic loop (mispredicted branches, or predication paying for all paths) and cheap for the generator (template copy indexed by opcode, no branch). Immediates never save bytes for once-used data.
- Sanity check (`p0-prototypes/round3/ro.c`, single runs, 16M ops, 6 kinds): run-once 6.0–6.3 cycles/op (5.2 generation alone) vs predicated interpreter 7.7–8.2 vs computed-goto 19.5. With predictable (single-kind) streams, computed-goto wins at 3.1.
- Question: on an op stream that is executed exactly once, with 16–32 opcode kinds of uneven cost, is generating and running throwaway native code ≥2× faster (cycles/op, generation included) than the strongest interpreter for that stream (the better of predicated/branch-free and computed-goto)?
- Inputs:
  1. synthetic random streams (16 and 32 kinds);
  2. a **real** op stream: the dynamic bytecode trace of Lua 5.4.6 running a fixed benchmark (opcodes plus operands, replayed against a register file). Opcode predictability there is whatever it is.
- Measure cycles/op, instructions, branch-misses and `ic_cache_fill_*` with perf, pinned. Tune the generator (pre-assembled templates, memcpy plus immediate patching).
- Rule: ≥2× on the real stream means "compile each op stream once, run once, discard" beats interpretation for once-executed code, so study where the crossover sits (predictability, ops per chunk). Under 2× on the real stream, kill it: the gain is too small to change how such streams are executed.
