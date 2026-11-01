# Cycle 2 plan

Chosen from the ideator's five proposals (see JOURNAL.md for cycle-1 evidence). Same format as cycle 1: one question, one number, one input, one rule.

## E4: Ring heap, forgetting without bookkeeping (assumption: memory must track liveness). Responds to E1.
- Question: what is the smallest ring-buffer heap S that keeps chibicc cc1's `parse.s` byte-identical? malloc bumps a pointer around a ring of S bytes and silently overwrites the oldest bytes; free is a no-op.
- How: an LD_PRELOAD allocator, binary search over S, comparing output with `cmp`. Overwriting live data must be detected, not left to luck, so each allocation carries a generation tag and is checked on access. If access can't be checked, overwrite freed memory with a poison pattern so that reuse corrupts output loudly.
- Number: S_min / 46.1 MB (total allocated).
- Rule: ≤10% means a lifetime-free ring allocator works here. Otherwise, long-lived objects are spread through allocation order and the idea is dead.

## E5: Dependency slices (assumption: compiling a function needs the whole TU resident). New ground: no shared state.
- Question: for each function in `parse.c`, how large is the smallest set of top-level preprocessed declarations that compiles it to the same asm (labels renumbered), as a fraction of the preprocessed TU's bytes? Report the median.
- How: the identifier-reference closure over top-level declarations in the `-E` output, then verify 20 functions by compiling slice+function with chibicc and diffing normalized asm.
- Rule: a median ≤10% means content-addressed slices could feed stateless workers. Otherwise, the header's mutually referencing structs pull in most of the TU.

## E6: Lex once across TUs (assumption: each TU is lexed from flat text, independently). New ground: compressed-domain.
- Question: across the 9 TUs of chibicc's self-host, what is the ratio of tokens lexed to tokens in distinct (file, line) source positions? A token at the same position, lexed again because a header is included again, counts once.
- How: instrument tokenize to log (file, line, column) per raw token over all 9 cc1 runs, then count total against distinct.
- Rule: ≥3× means a shared immutable token store across TUs is worth building next. Otherwise, redundancy across TUs is small.

Deferred: output-addressed asm keys over history (the label counter is a known global; cheap to run later), and search over real peephole windows.
