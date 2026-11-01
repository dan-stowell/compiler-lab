# Cycle 1 plan (narrowed, revised after review)

Each experiment answers one question with one number on one input, against one decision rule: no middle band. Each lives in `experiments/eN-*/` with a `run.sh` that reproduces it from scratch and a `RESULTS.md` labelling each number as measured or estimated.

Workload: chibicc @ 90d1f7f, fetched by `experiments/common/get-chibicc.sh`.

## E1: Gate for "tokens as generators" (assumption: memory holds values)
- Question: of all bytes the chibicc `-cc1` process allocates while compiling `parse.c`, what fraction are Token structs that a Node, Obj or Type still points to after parsing finishes? Only those tokens are ones that regeneration could save but plain free() could not.
- How: an LD_PRELOAD or linked wrapper over malloc/calloc/realloc/free, attributed to the cc1 child, plus tagging Token allocations in chibicc. Also report Token bytes overall, AST bytes, the output buffer, and peak RSS.
- Rule: if retained tokens are ≥25% of bytes, build re-lex-from-offset over `chibicc -E` output next cycle. Otherwise, regenerating tokens is dead on this workload.
- Changes from the first plan: dropped the G1 streaming control (peak heap is reached before codegen, so freeing after codegen frees nothing before the peak) and dropped byte-identical assembly checks. Regeneration is deferred until the gate says it could matter. Raw source offsets can't rebuild macro-expanded tokens, hence the `-E` buffer.

## E2: Merkle ripple over history (assumption: linking needs a global symbol table)
- Question: over chibicc's 316 commits, what is the median fraction of top-level functions whose closure key changes from one commit to the next? The closure key is a hash of the function body plus the closure keys of the functions it calls, with mutually recursive groups (SCCs) hashed as one unit.
- Also report the plain body-hash change fraction and the largest SCC's size, for context.
- How: a token-level splitter over the raw `.c` files (no preprocessing). A call is an identifier matching another top-level function name.
- Rule: if the median closure ripple is ≤10%, Unison-style whole-closure addressing is viable for C here. Otherwise, closure keys over-invalidate and the next step is keys built from prototypes.
- Changes from the first plan: SCCs handle recursion, and the asm soundness check and miss-bytes metric are cut.

## E3: Search instead of derivation (assumption: output structurally follows source); replaces the HashLife rewriter
- Question: for a fixed set of 200 random expressions (≤4 binary ops from + - * & | ^ over three 4-bit inputs x, y, z, plus 4-bit constants, with mod-16 semantics), how long is the shortest stack-VM program found by search, against a tree-walking compiler's output?
- How: breadth-first enumeration of VM programs, deduplicating programs whose results agree on all 4096 inputs (enumerate distinct behaviours, not programs). Each result is checked exhaustively against the expression.
- Report mean length ratio (search/derived), search CPU, and peak memory, with a 10-minute cap.
- Rule: if the mean length ratio is ≤0.75, search finds structure that derivation misses. Otherwise, search buys little at this scale.
- Change from the first plan: the HashLife rewriter was cut by all three reviewers. Its success was guaranteed by construction, and it overlapped E2.

Each experiment gets one independent verifier rerun. Results and failures go in `journal/JOURNAL.md`.
