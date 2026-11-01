# Cycle 1 plan (narrowed): three experiments, one question each

Each experiment answers a single yes/no question with one number, on one input. No sweeps or extra variants until that number is in. Each has a `run.sh` that reproduces it and a `RESULTS.md` labelling every number as measured or estimated.

Workload: chibicc @ 90d1f7f (https://github.com/rui314/chibicc).

## E1: Can tokens be regenerated instead of stored? (assumption: memory holds values)
- Question: if chibicc keeps only a 4-byte source offset per token and re-lexes on every access (no cache), how much does total allocation drop when it compiles `parse.c`, and at what CPU cost?
- Single variant against unmodified chibicc. Correctness check: identical `.s` output for `parse.c`.
- Falsified if bytes allocated drop by <2×, or CPU goes up by >20×.

## E2: How much of a commit is unchanged at function granularity? (assumption: compilation is per-file)
- Question: across chibicc's commit history, what fraction of top-level function definitions (hashed after whitespace normalization) are byte-identical to one in the previous commit?
- One script over `git log`; output one median and one histogram. No Merkle keys, no per-function compiling yet.
- Falsified if the median reuse is <80%; then function-level content addressing isn't worth pursuing on this workload.

## E3: Does a global memo make incrementality free? (assumption: incrementality needs dependency tracking)
- Question: in a ~200-line hash-consed term rewriter with a global (term-hash → normal-form) memo, how many rewrites does re-normalizing take after changing one leaf of a large term, as a fraction of the cold run?
- One term family (a balanced arithmetic expression tree with 2^16 leaves), one edit. No compiler-as-rules, no VM yet.
- Falsified if warm rewrites are >10% of cold, or the memo is >10× the size of the result.

Each gets one independent verifier rerun. Results and failures go in `journal/JOURNAL.md`.
