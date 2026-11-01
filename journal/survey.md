# Survey: Unusual Compilation & Computation Mechanisms

Format: **mechanism** / *trade* / limitation.

## A. Content-addressing, memoization, incrementality
- **Unison** — definitions identified by hash of their AST (names are metadata); codebase is an append-only DAG, so no rebuilds, renames are free, remote code shipping is "send missing hashes." *Trade:* hashing CPU for zero redundant compilation/transfer. Limit: tooling/ecosystem friction; mutually recursive hashing and effects complicate the model.
- **Nix / Bazel** — builds as pure functions keyed by input hashes; remote cache + remote execution. *Trade:* hashing for skipped work and shared artifacts. Limit: hermeticity is expensive to enforce; input-addressed (not output-addressed) Nix misses early cutoff (CA-derivations fix partly).
- **Salsa / Adapton / rustc query system** — demand-driven memoized query graph with red-green revalidation and early cutoff. *Trade:* dependency-tracking memory for recompute avoidance. Limit: memory for the trace often dominates; fine granularity costs overhead.
- **"Build Systems à la Carte" (Mokhov et al.)** — decomposes builds into scheduler × rebuilder (dirty bit, verifying/constructive traces, deep traces). *Trade:* explicit spectrum of trace-storage vs recompute. Limit: monadic (dynamic) dependencies break static analysis.
- **Memoization at scale / hash-consing** — maximal sharing of structurally equal values (unique table); O(1) equality. *Trade:* hash-table CPU/memory for dedup. Limit: GC of weak tables, hash contention in parallel settings.
- **Deterministic replay (rr, Hermit, Antithesis)** — record only nondeterministic inputs, regenerate execution. *Trade:* replay CPU for storing tiny logs instead of traces/states. Limit: hard on multicore/shared memory; syscall surface.

## B. Reduction machines & strange evaluators
- **Turner SKI combinator graph reduction (SASL/Miranda)** — compile λ to fixed combinators, reduce graph in place; laziness + sharing for free. *Trade:* code size blowup (fixable via bracket abstraction/supercombinators) for a trivially simple machine. Limit: slow vs. compiled STG/G-machine.
- **Interaction nets / optimal reduction (Lafont, Lamping, HVM/Bend)** — local, confluent graph rewrites; optimal (Lévy) sharing even under λ; inherently parallel. *Trade:* more bookkeeping (fan/bracket nodes) for asymptotic speedups on some terms. Limit: oracle (bookkeeping) cost can be exponential; HVM restricts to affine/"lazy dup" fragments; real-world wins are narrow.
- **Linear logic / Geometry of Interaction machines** — evaluate by token-passing along proof-net paths; constant-space-per-node, no graph copying. *Trade:* massive recomputation for near-zero memory (GoI "space-efficient" interpretations, IAM). Limit: time blowup can be exponential; Dal Lago's work bounds it for some classes.
- **Dataflow (Id, Monsoon, MIT Tagged-Token)** — instructions fire on token arrival; I-structures give write-once sync. *Trade:* token-matching hardware/memory for latency tolerance. Limit: token store explosion, poor locality; died to OoO superscalar (which is dataflow internally).
- **Tensor/stream ISAs (Cerebras, Tenstorrent, TPU systolic, CGRAs, Stream-K)** — compile to spatial dataflow; data streams past stationary compute. *Trade:* recompute/rematerialization for bandwidth. Limit: irregular control flow and compile times.
- **Forth / threaded code** — program is a list of addresses; inner interpreter `NEXT`. *Trade:* small CPU overhead for extreme code density. Limit: weak static typing/optimization.
- **Joy / concatenative** — programs are functions composed by juxtaposition; quotation as data; no variables. *Trade:* algebraic rewrite-friendliness for stack-shuffling cost. Limit: readability; dataflow hidden.
- **Kernel / fexprs (Shutt)** — operatives receive unevaluated operands + environment; first-class macros. *Trade:* flexibility for loss of static compilability. Limit: "theory of fexprs is trivial" (Wand) — almost no equational reasoning.
- **Urbit Nock + jets** — tiny combinator spec (~12 rules); hot formulas matched by hash and replaced by native "jets." *Trade:* spec purity vs. trusting jets equal spec. Limit: jet mismatch bugs; slow without jets.
- **Smalltalk images** — persist the whole heap; no build, just snapshot. *Trade:* storage of live state vs. regeneration from source. Limit: provenance, diffing, reproducibility.
- **Reversible computing (Janus, Bennett's trick)** — invertible ops; uncompute garbage. *Trade:* time (≈2× or more) and pebbling for near-zero erasure/space. Limit: hardware pay-off (Landauer) still speculative.
- **HashLife (Gosper)** — quadtree + hash-consing + memoized macro-steps; skips time exponentially in regular worlds. *Trade:* memo memory for astronomically fewer steps. Limit: chaotic patterns blow the cache.

## C. Program transformation & optimization
- **Supercompilation (Turchin)** — drive the program symbolically, fold on homeomorphic embedding, generalize. *Trade:* compile time for removal of intermediate structure. Limit: code explosion, termination heuristics fragile.
- **Partial evaluation / Futamura projections** — specialize interpreter to program → compiler; specializer to interpreter → compiler-generator. *Trade:* specialization time + code size for runtime speed. Limit: binding-time analysis brittle; Truffle/Graal is the practical survivor.
- **Tracing / meta-tracing JIT (Self, PyPy/RPython)** — record hot loop traces (of the interpreter itself in RPython) and compile them; Self's maps + PICs. *Trade:* warmup CPU/memory for speed. Limit: trace explosion on branchy code; warmup cliffs.
- **Equality saturation (egg, egglog)** — apply all rewrites non-destructively into an e-graph, extract best. *Trade:* memory of the e-graph for phase-ordering freedom. Limit: e-graph blowup; extraction is NP-hard.
- **Superoptimization (Massalin, STOKE, Souper)** — search (MCMC/SMT) for the shortest equivalent sequence. *Trade:* huge offline CPU for tiny/fast code. Limit: short windows only; verification cost.
- **Datalog compilers (Soufflé, Doop, Flix, Datalog-based analyses)** — compile relational fixpoints to specialized C++ with semi-naïve evaluation. *Trade:* index memory for fast incremental fixpoint. Limit: no aggregates/functions without extensions; large intermediate relations.
- **Genetic programming / program synthesis (Koza, FlashFill, Sketch, DreamCoder)** — search for programs meeting spec; DreamCoder grows a library of abstractions ("wake-sleep"). *Trade:* search CPU for compact programs. Limit: scaling beyond small programs.
- **Neural compilers / LLM superoptimizers / learned cost models (MLGO, TVM Ansor)** — learned heuristics or direct translation. *Trade:* training compute for better decisions. Limit: no correctness guarantees without verification.
- **Self-improving compilers (Ershov, Jones self-application, MLGO)** — compiler specializes/optimizes itself. Limit: diminishing returns past bootstrap.

## D. Computation as compression
- **Kolmogorov / programs-as-compressors** — shortest generator = best compression. Demoscene **kkrunchy/Crinkler** use context-mixing on x86 code; 4k intros *regenerate* textures/music procedurally. *Trade:* CPU at load for 100–1000× smaller binaries. Limit: K is uncomputable; decompression time ad hoc.
- **ROM-less / regenerable binaries** — procedural content, recompute tables at boot, JIT from compressed IR (e.g. .kkp, Wasm streaming compile). Limit: startup latency.
- **zstd dictionaries / grammar compression (Re-Pair, Sequitur, SLPs)** — SLPs are straight-line programs generating the string; many queries (equality, pattern match, access) run directly on the grammar in poly(grammar) time. *Trade:* algorithmic complexity for never decompressing. Limit: finding smallest grammar is NP-hard; edits are hard.
- **Compressed-domain/homomorphic compression (succinct structures, FM-index, column stores on RLE/dict, "compressed linear algebra")** — compute on compressed representation. Limit: each operator needs a bespoke algorithm.
- **BDDs/ZDDs** — canonical, hash-consed DAGs representing Boolean functions/set families; ops are memoized graph algorithms. *Trade:* unique-table memory for exponential compression. Limit: variable ordering; some functions (multipliers) explode.

## E. Distribution & modules
- **CRDT-based code (Unison sync, Automerge for ASTs, Hazel structure editors)** — mergeable program edits without central authority. Limit: semantic conflicts not resolved by syntactic merge.
- **WebAssembly Component Model** — typed, language-neutral components + canonical ABI; capability-safe linking. *Trade:* copy/lifting cost at boundaries for portability. Limit: no shared memory across components; still young.
- **Content-addressed/decentralized execution (IPFS/IPVM, Bacalhau, Golem, Holochain)** — move compute to data by CID. Limit: trust in results (needs verification/replication or proofs).
- **Verifiable computation (SNARK/STARK zkVMs: RISC Zero, SP1)** — prove execution; verifiers skip recompute. *Trade:* 10^4–10^6× prover CPU for tiny verification and bytes. Limit: prover cost.

## Under-explored intersections
1. **E-graphs × grammar compression** — e-graphs and SLPs are both hash-consed DAGs; equality saturation over a corpus could yield *semantic* compression (smallest equivalent generator), and Re-Pair could discover rewrite rules/library abstractions (cf. babble/DreamCoder).
2. **Interaction nets × content addressing** — hash net fragments, ship/memoize reductions globally (HashLife for λ-calculus); local confluent rewriting needs no global consistent state.
3. **GoI / reversible evaluation × storage-constrained execution** — deliberately recompute instead of store: pebbling-optimal schedules as a compiler pass, choosing per-value "store vs regenerate vs fetch."
4. **Superoptimization in idle cycles × content-addressed cache** — a global, hash-keyed database of proven-optimal fragments (Souper-at-scale, Nock-jet style replacement with SMT proof certificates).
5. **Supercompilation × compressed data** — specialize algorithms to the *grammar* of a specific compressed dataset, automatically deriving compressed-domain operators instead of hand-writing them.
6. **Partial evaluation × deterministic replay** — specialize programs to recorded input logs; "replay certificates" let caches store only the seed + specialized generator.
7. **Datalog/incremental queries × CRDTs** — monotone (lattice) Datalog is CRDT-shaped; a compiler built as a distributed monotone fixpoint could run without global state.
8. **Learned models as decompressors for code/IR** — neural/context-mixing predictors over IR (Crinkler for IR) plus verified regeneration: store prompts/seeds + checksum, regenerate artifacts on demand.
