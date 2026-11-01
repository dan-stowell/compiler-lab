Phase 1 argues for semantic experiments with aggressive controls: ordinary reclamation defeated regeneration, cheap simplification defeated search, and decoding cost ~70× native. Below are six proposed computational worlds—not claims of historical novelty. Their movement advantages are hypotheses; a conventional implementation exploiting the same structure may defeat them.

**Shared measurement contract.** Start from identical compact inputs and require identical semantic outputs. Include initialization, description construction, instruction generation, execution, recomputation, and output. Report bytes separately across storage↔RAM, RAM↔LLC, LLC↔private caches, and instruction-cache boundaries; add network bytes where applicable. Use physical I/O accounting, available memory-controller/cache events, and `perf` instructions, cycles, instruction-cache misses, and iTLB misses. Counters are architecture-dependent: distinguish measured bytes from inferred line transfers and mark unavailable events. Count generated code bytes and repeated evaluations explicitly. Run cold and warm, including all repetitions. Each experiment’s number is baseline/model traffic at its named boundary; advance at ≥100× reduction, regardless of slowdown.

**1. Programs as trajectories through a rule space**

- **Core:** `step(seed, cursor, observation) → (action, cursor′)`. Actions are arithmetic, observation, or termination. Only the current action exists; observations can alter which rules subsequently exist. There are no instruction addresses or stored future instructions.
- **One day:** Implement a bounded arithmetic rule generator and executor in C; compare interpreting actions with generating small executable batches.
- **Operation/control:** Execute a billion-step, compactly generated decision trajectory; target ≥100× less instruction-fetch traffic than an expanded native decision graph. Mandatory stronger control: a native loop implementing the generator directly. If it ties, the model offers expressiveness, not a movement discovery.
- **Failure:** Incompressible action sequences make the seed effectively a stored program; frequent generation magnifies E12.
- **Scaling question:** Can observation-dependent generators remain compact while expressing useful irregular control?

**2. Collections as questions with executable answers**

- **Core:** `collection = {x | predicate(x)}`; `member(C,x)`, `count(C,region)`, and `split(region)` are primitive observations. A collection contains a predicate and certified regional counting rules, never elements.
- **One day:** Implement integer boxes, congruence predicates, intersection, and exact counting by recursive partition plus arithmetic certificates.
- **Operation/control:** Count a structured join over two trillion-element logical collections; target ≥100× less RAM traffic than compressed columnar execution. Also challenge it with indexed and algebraically rewritten SQL—the strongest control may eliminate enumeration too.
- **Failure:** Random predicates defeat certificates and force exhaustive enumeration.
- **Scaling question:** How expressive can predicates become before discovering regional answers costs more than scanning?

**3. Memory budgets as meaning**

- **Core:** `Γ ⊢ e : T @ B` means evaluation fits within B resident bytes. Live values consume linear byte credits; dropping returns credits. A recipe can be reevaluated, but cannot silently spill. Insufficient credits make a program invalid.
- **One day:** Build a straight-line DAG language, conservative footprint checker, and evaluator that deletes values and replays ancestors within a fixed arena; account for stack and recipes.
- **Operation/control:** Evaluate a deep, narrow-output dependency graph under a cache-sized budget; target ≥100× less DRAM traffic than materializing every node. Strong control: checkpointing with an optimized pebbling schedule.
- **Failure:** High-expansion DAGs demand exponential recomputation.
- **Scaling question:** Can compositional budget proofs remain useful without solving global scheduling problems?

**4. Programs as unretained conversations**

- **Core:** `producer.request(q, continuation)` immediately answers or issues another request. Composition connects requests directly; intermediate syntax, streams, and trees have no identity or storage operation. Repetition restarts the producer.
- **One day:** Implement procedural grammar expansion connected directly to a parser, evaluator, and reducer through bounded continuations.
- **Operation/control:** Validate and aggregate a terabyte-sized generated document without emitting tokens or a tree; target ≥100× less DRAM traffic than a materializing parser pipeline. Strong control: a hand-fused streaming parser.
- **Failure:** Backtracking and cross-document references repeatedly regenerate prefixes.
- **Scaling question:** Which transformations compose with bounded continuation state, especially when consumers request incompatible traversal orders?

**5. Communication replaced by local universes**

- **Core:** `world(seed, coordinate, time)` is pure. Each observer independently reconstructs its causal cone; cross-observer mutable references do not exist. Messages carry initial conditions and final observations only.
- **One day:** Implement a deterministic cellular system across processes; regenerate overlapping cones independently with bounded local memoization.
- **Operation/control:** Answer sparse, distant future-cell queries; target ≥100× fewer network bytes than distributed timestep simulation with halo exchange. Strong control: communication-avoiding temporal blocking and query-specific cone evaluation.
- **Failure:** Dense queries or rapidly expanding cones duplicate nearly the entire simulation.
- **Scaling question:** Where does causal-cone overlap overwhelm the communication savings?

**6. History as an inverse law**

- **Core:** State evolves only through bijections: `s′ = F(s)` and `s = F⁻¹(s′)`. History queries walk backward; irreversible observations must explicitly retain their lost information.
- **One day:** Build reversible integer primitives and a simulator supporting backward traversal and historical checksums.
- **Operation/control:** Inspect every previous state in reverse order; target ≥100× less storage traffic than snapshot logging. Strong control: compressed deltas and checkpoint/replay.
- **Failure:** Irreversible external inputs require logs; scattered historical queries require enormous replay.
- **Scaling question:** Can useful computation expose enough reversible structure that retained entropy stays small?