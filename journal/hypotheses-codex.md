These are deliberately risky predictions, not established results. Use a tiny expression-and-function compiler targeting a stack VM as the shared testbed. Each experiment should take less than one day once that baseline exists. Require identical program outputs; measure CPU and wall time separately. Count storage/network bytes directly and distinguish instrumented logical memory traffic from hardware DRAM traffic.

1. **An IR can be a recipe rather than a data structure.**  
   **Claim:** Replacing stored IR nodes with source coordinates and reconstruction recipes will cut peak memory by **10×** and logical memory traffic by **3×**, within **100× CPU**.  
   **Challenges:** Intermediate results should survive until their last consumer.  
   **Experiment:** Compile generated programs with repeated optimization passes, comparing a materialized tree against nodes reconstructed on every access. Sweep recipe-cache capacity from zero upward. Measure peak memory, node-read/write bytes, CPU, and wall time. Failure would expose whether reconstruction secretly requires comparable state or moves more bytes than it saves.

2. **A compiler can search for its output instead of deriving it.**  
   **Claim:** For functions containing at most six arithmetic operations, enumerating VM programs and checking equivalence will produce **4× smaller executables** with **8× less working memory**, within **1,000× CPU**.  
   **Challenges:** Compilation must preserve a structural correspondence between source and output.  
   **Experiment:** Use three 4-bit inputs, allowing exhaustive equivalence checks. Compare an enumerator retaining only its current candidate against parsing, optimizing, and emitting an AST. Measure executable bytes, peak memory, CPU, and wall time; cap each search at ten minutes and count timeouts as failures.

3. **Exhaustive rescanning can replace symbol tables.**  
   **Claim:** Resolving each identifier by rescanning source will reduce compiler working memory by **20×** and logical memory traffic by **2×**, within **200× CPU**, for declaration-heavy programs.  
   **Challenges:** Names require a persistent index.  
   **Experiment:** Implement nested scopes in a tiny language; compare hash-table lookup against repeated source scans with constant-sized scope counters. Generate varying declaration counts, identifier lengths, and reference densities. Measure memory, bytes read/written, CPU, and wall time. The traffic prediction is intentionally aggressive: finding its crossover would be informative even if it fails.

4. **Replay can be cheaper than retaining compiler state.**  
   **Claim:** Keeping only an input and transformation-choice log, then replaying from the beginning for later passes, will reduce peak memory by **16×** and spill-file I/O by **90%**, within **50× CPU**.  
   **Challenges:** Compiler progress must be embodied in a mutable current IR.  
   **Experiment:** Chain twelve deterministic rewrites over generated expressions. Compare retained snapshots, disk-backed snapshots, and replay under the same memory limit. Measure peak memory, storage bytes, CPU, and wall time, including replay. Unlike node regeneration, this tests reconstruction of whole historical states.

5. **The optimizer should live beside the stored program.**  
   **Claim:** Sending rewrite rules to a storage-side worker will reduce compiler network traffic by **30×**, within **3× total CPU**, when optimization produces a small result from a large stored IR.  
   **Challenges:** Data must travel to a central compiler process.  
   **Experiment:** Use two local processes connected through a metered socket: one owns serialized IR chunks, the other requests optimization. Compare fetching chunks against executing a restricted rewrite interpreter at the owner. Sweep output selectivity and cross-chunk references. Measure socket bytes, storage reads, total CPU, and wall time.

6. **Duplicate computation can replace distributed agreement.**  
   **Claim:** Four compiler workers exchanging immutable results and tolerating duplicate work will use **10× fewer coordination bytes**, with at most **2× CPU** and **25% worse wall time**, than centralized task claiming.  
   **Challenges:** Distributed compilation requires globally unique task ownership.  
   **Experiment:** Partition compilation into deterministic function-level tasks. Compare a coordinator against independent workers choosing tasks from local knowledge and gossiping result hashes. Inject delayed messages and worker restarts. Verify complete, identical output; measure coordination bytes separately from result bytes, duplicated CPU, and wall time.

7. **Programs can be distributed as algebraic differences.**  
   **Claim:** Transmitting an invertible set-reconciliation sketch of content-addressed compiler nodes will reduce update traffic by **20×** versus exchanging complete node inventories, within **10× CPU**, when replicas differ by 1%.  
   **Challenges:** Discovering missing computation requires enumerating everything present.  
   **Experiment:** Build two overlapping stores from successive compiler inputs. Compare full hash-list exchange with a small invertible Bloom lookup table followed by missing-object transfer. Sweep divergence and sketch capacity; include failed decoding and retries. Measure all network bytes, CPU, and wall time. Also compare a Merkle-tree inventory to expose a weak baseline.

8. **A dormant compiler can rewrite itself into a specialist.**  
   **Claim:** Spending one idle hour synthesizing dispatch shortcuts from previous compilations will reduce CPU on a held-out workload by **30%**, while growing the compiler representation by less than **5%**.  
   **Challenges:** Compiler implementation is fixed; only its input programs improve.  
   **Experiment:** Instrument a table-driven rewrite engine, synthesize fused rule sequences, and install only equivalence-checked sequences. Train on one program family and test on unseen families. Measure training CPU, executable/table bytes, compilation CPU, and wall time. Report the number of compilations required to repay training, including regressions.

9. **Predicting future computation can beat caching past computation.**  
   **Claim:** With an equal **32 MiB** artifact budget, speculative compilation of predicted next edits will reduce foreground CPU by **40%** versus an LRU result cache, using at most **20× background CPU**.  
   **Challenges:** Useful retained work must correspond to an input already requested.  
   **Experiment:** Generate an edit trace with recurring rename, constant-change, and expression-extension patterns. Train an edit predictor on the first half; use idle cycles to compile predicted variants. Evaluate the second half without lookahead. Measure foreground/background CPU, artifact bytes written/read, wall time, and wasted predictions.

10. **Compiler optimization can be demand-driven evaluation.**  
    **Claim:** Reducing a lazy graph only as code emission demands it will cut logical IR traffic by **10×** and CPU by **3×** on programs where 90% of definitions are unreachable.  
    **Challenges:** Optimization is a sequence of whole-program passes.  
    **Experiment:** Implement constant folding, inlining, and dead-definition removal as both eager passes and memoized demand-driven reductions. Include an eager baseline that performs reachability first. Vary reachability and recursive dependencies. Measure node accesses, peak memory, CPU, and wall time. Failure against the stronger baseline would distinguish a new evaluation mechanism from ordinary dead-code elimination.

11. **Scheduling can matter more than sharing.**  
    **Claim:** A compiler that recomputes cheap shared expressions according to a bounded-memory graph-pebbling schedule will use **8× less peak memory** and **3× fewer spill bytes**, within **20× CPU**.  
    **Challenges:** Common subexpressions should always be computed once.  
    **Experiment:** Generate optimization dependency DAGs with diamonds and long-lived shared nodes. Compare compute-once scheduling against a greedy pebbling scheduler that discards and regenerates values. Give both the same memory cap and spill mechanism. Measure peak resident intermediates, storage traffic, recomputation CPU, and wall time. Sweep node cost to find where sharing becomes essential.

12. **Optimization history can make the executable smaller.**  
    **Claim:** Shipping a compact seed program plus an inverse-rewrite script will reduce executable storage and transfer bytes by **5×**, with reconstruction costing less than **100× ordinary load CPU**.  
    **Challenges:** The executable should directly encode the instructions that run.  
    **Experiment:** Produce VM programs with systematic unrolling and specialization. Encode them as explicit bytecode, conventionally compressed bytecode, or a seed plus parameterized expansion script. Include the decoder’s amortized size and execute reconstructed outputs. Measure total bytes, reconstruction CPU, peak memory, and startup wall time; generic compression is the decisive control.

13. **Approximate intermediates can support exact compilation.**  
    **Claim:** An 8-bit approximate abstract interpreter followed by exact verification of proposed rewrites will reduce analysis-state memory by **8×**, while retaining **80% of successful rewrites** within **3× CPU**.  
    **Challenges:** Every internal compiler fact must itself be exact or sound.  
    **Experiment:** Quantize range estimates aggressively, allow them to suggest unsafe rewrites, and verify every suggestion exhaustively for a small finite-width language. Compare against exact range analysis. Measure state bytes, verification CPU, wall time, accepted rewrites, and final bytecode size. Any unverified rewrite reaching output falsifies correctness immediately.

14. **Structural identity can emerge through rewriting.**  
    **Claim:** Canonicalizing expressions before content-addressed lookup will produce **4× more reusable compilation results** and **3× less result-transfer traffic**, within **10× CPU**, across syntactically different programs.  
    **Challenges:** Cache identity is a property of input bytes rather than a computed semantic representation.  
    **Experiment:** Generate variants using variable renaming, associative regrouping, and commutative operand swaps under fixed-width integer semantics. Compare source hashing, syntax-tree hashing, and bounded canonical rewriting. Test unrelated programs as well as generated variants. Measure reuse, transferred bytes, canonicalization CPU, and wall time; include canonicalizer versioning in keys.

15. **Recomputation can replace distributed result storage entirely.**  
    **Claim:** Workers retaining only task recipes and small verification hashes will reduce durable storage by **100×** and recovery network traffic by **10×**, within **100× recovery CPU**, relative to replicated compiled artifacts.  
    **Challenges:** Fault tolerance requires durable copies of computed results.  
    **Experiment:** Compile deterministic functions across four worker processes, then delete selected worker state. Compare three-way artifact replication against replicated input recipes with on-demand regeneration. Include recipe dependencies, compiler version, and source bytes in accounting. Measure durable bytes, recovery traffic, CPU, and time to serve the first and all requested artifacts.