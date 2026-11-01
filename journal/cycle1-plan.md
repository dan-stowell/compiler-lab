# Cycle 1 plan: three experiments, three different assumptions

Workload: chibicc pinned at 90d1f7f (https://github.com/rui314/chibicc), fetched by `experiments/common/get-chibicc.sh`. Every experiment has a `run.sh` that reproduces its numbers from scratch, and writes `RESULTS.md` labelling each number as measured or estimated.

## E1 — Values → generators (attacks "memory holds values")
Hypothesis 5 (ideator) and codex 1. Prediction: making tokens regenerable from (source offset) and dropping each function's AST and tokens after codegen cuts chibicc's peak heap ≥10× at ≤5× CPU on its own self-compile.
- Steps: (a) instrument chibicc's allocations (LD_PRELOAD counter or a calloc wrapper) and attribute peak bytes to tokens / AST nodes / types / other; (b) variant G1 frees per-function AST and tokens after codegen (the conventional "streaming" control); (c) variant G2 removes the stored token list entirely: a token is (offset), and its fields are recomputed by re-lexing when accessed, optionally with a cache of K tokens. Sweep K to get a CPU-vs-peak-memory curve.
- Correctness: stage2 assembly from each variant is byte-identical to the baseline's for all 9 TUs, and `make test` passes.
- Falsified if G2 cannot reach ≥3× below G1 without >5× CPU, i.e. if regeneration buys nothing beyond ordinary freeing.

## E2 — Content-addressed functions over history (attacks "compilation is per-file and linking is global")
Hypotheses 4 and 11. Prediction: across chibicc's 316 commits, ≥95% of top-level function bodies (normalized, preprocessed) are reused from the previous commit, and with Merkle references (a function's key includes its callees' keys and the types it uses) the median commit dirties ≤3% of functions.
- Steps: for each commit, extract top-level definitions (the chibicc parser itself, or a small tokenizer-based splitter), hash with and without callee closure, and record hit rates and the ripple size. Then compile per-function with chibicc (emit asm for one function) and check that identical keys give identical asm (key soundness).
- Metric: memo hit rate per commit, ripple distribution, bytes that would cross a network for an incremental build (miss bytes) against whole-object fetch.
- Falsified if the Merkle median ripple is >20% (edits to shared types or helpers ripple to everything), or if keys are unsound (same key, different asm).

## E3 — HashLife for term rewriting (attacks "incrementality needs explicit dependency tracking", and "repeated structure means repeated work")
Survey intersections 2 and 1, ideator 16. Build a ~500-line, from-scratch, hash-consed term rewriter where every (term-hash → normal-form-hash) is memoized in a global table, plus a tiny compiler written as rewrite rules (an expression language → stack-VM code). Prediction: after a one-line edit, recompiling performs ≤2% of the cold rewrites with no build system; on inputs with repetition (e.g. a program generated with 2^n self-similar structure) work is O(n), not O(2^n).
- Metric: rewrite steps, memo table bytes, wall time, cold against warm, and the memo table's size against storing the outputs.
- Correctness: the VM runs the compiled output and matches a reference interpreter on generated programs.
- Falsified if the warm/cold ratio is >10%, or if the memo table outgrows the outputs it saves by more than 10×.

Each experiment gets an independent verifier rerun, and results go in `journal/JOURNAL.md`, failures included.
