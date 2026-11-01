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

## E9: Executable compression (assumption: compact code must be decompressed before it runs)
- Background (ideator, single run, unverified): chibicc's stage2 stack asm is 79,399 instructions. `xz -9e` of the clang `-Oz` .text is 29,976 B. A crude Re-Pair run within basic blocks gives about 60 KB (2.0×). The LLVM machine outliner gains only 0.9%.
- Question: can a representation that runs directly with no sequential decoding be as small as xz? Directly means fixed-width symbols, grammar rules executed as call-threaded subroutines, and random access at every branch target.
- Representation to try: a Re-Pair grammar over opcode skeletons, with operands in a separate fixed-width stream and a table of distinct instructions. Every byte needed to execute counts: rule bodies, symbol stream, operand stream, instruction table, and any branch-target index into the operand stream.
- Input: chibicc stage2's self-compiled asm (`.text` only, labels resolved to symbol indices).
- Number: total encoded bytes / 29,976 B (xz -9e of the -Oz .text). Report xz of the same stack asm as a second reference.
- Rule: ≤1.25× means an executable form at entropy-coder size exists, so the next step is building the interpreter and measuring speed. Above 1.25×, executability costs more than entropy coding saves.
- Size only this cycle; the round-trip check (expand the grammar back and compare with the original instruction sequence) is required.
