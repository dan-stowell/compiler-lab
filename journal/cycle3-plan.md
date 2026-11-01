# Cycle 3 plan (revised after review)

E5 found that under chibicc a function compiles to identical asm from its body, a ~2.5% context slice, and one "defined here" bit per callee. An architecture challenge argued that slice-keyed caching on chibicc re-derives Bazel, rustc queries and ThinLTO, and that the one possibly new claim is "a small, fixed slice is enough". That has to survive an optimizing compiler.

## E8: How big must a slice be under clang -O2?
- Input: Lua 5.4.6 `lparser.c`, preprocessed with the project's flags. Compiler: clang 18, `-O2 -fno-pic` (fixed explicitly).
- Targets: externally visible functions only. Static functions also depend on their callers (interprocedural constant propagation, dead-argument elimination) and can't be emitted standalone without changing their code. Report how many static functions are excluded for that reason.
- Procedure: for each target, try depth d = 0, 1, 2, … until its normalized asm matches the full-TU output. At depth d, the slice holds the target, its context declarations, and the **bodies** of callees within d call levels (prototypes beyond that), in original order. Record the smallest matching d and that slice's size in tokens as a fraction of the TU. A target that never matches short of the full TU counts as 100%. The full TU itself is the normalization control (it must match 100%).
- Normalization: rename private labels bijectively (.LBB, .Ltmp, .LJTI, .LCPI, .L.str, .Lfunc_end) while keeping reference identity. Compare the function's text plus the referenced rodata (jump tables, constants, strings), extracted by label. Strip only enumerated debug directives (.loc, .file, .cfi_*), nothing else.
- Number: median smallest-matching slice size, as % of TU tokens. Also report the distribution of d.
- Rule: median ≤10% means a dependency-bounded slice survives -O2, which is worth an edit-based test next. Above 10% means the optimizer couples functions too tightly; record that the direction reduces to ThinLTO-style summaries.
- No "defined here" level: under clang a body-less static declaration changes codegen (`@PLT`, `-Wundefined-internal`), so it isn't a faithful summary.

## E9: Executable compression (assumption: compact code must be decompressed before it runs). Revised after review.
- Question: can a grammar form of chibicc stage2's own instruction sequence, executed without expanding it, be as small as xz of that same sequence? Execution here means sequential symbol dispatch, rules within basic blocks, and every branch target entered with no decoding of preceding code.
- Denominator: `xz -9e` of stage2's `.text`, the identical instructions (about 32.5 KB; pin the file and its hash). clang `-Oz` (≈30 KB) is context only.
- Step 1, the decider: a lower bound = final symbols × ⌈log2 alphabet⌉ + operands at a fixed width per skeleton + instruction/skeleton table + label table (symbol position, operand offset), from a Re-Pair run that keeps every label and operand distinct. Report payload-only, plus the handler cost separately (one native handler per distinct skeleton, or a fixed generic interpreter).
- Rule: if the lower bound is >1.25× the denominator, this representation loses to entropy coding: record it and stop. Only if ≤1.25×, build the full encoding with a round-trip check and a branch-entry check.
- A failure rejects this representation, not executable compression in general.
