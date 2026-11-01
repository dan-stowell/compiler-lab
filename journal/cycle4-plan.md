# Cycle 4 plan (revised after review)

## E10: Hidden dependencies of learned slices (assumption: a cached per-function result needs a sound, explicit dependency set)
- Background: E8 found that a function's clang -O2 output is reproduced by a median 6.5% slice. A real system would learn each slice once and reuse it. That is unsound if the full-TU output changes because of code outside the slice (E8's close_func effect: a new caller of a static callee changes inlining).
- Input: lua/lua git history, first-parent, commits that touch core `.c`/`.h`. Exclude `onelua.c`, `ltests.c`, `lua.c` and `luac.c`. clang 18, `-O2 -fno-pic`, flags pinned. E8's slicer and comparator. Commits that fail to compile are recorded, not silently dropped.
- Cache key: the token sequence of the slice **re-extracted at N+1 with depth d frozen from N** (definition order kept), plus compiler and flags. Renamed or deleted targets are misses. Targets whose slice was the whole TU stay in.
- Hits counted: only in TUs whose `-E -P` token stream changed between N and N+1. Unchanged TUs are skipped, with determinism spot-checked on a few. For each counted hit, compare the full-TU normalized asm at N+1 against N.
- Sample: first a token-diff-only pass to estimate n (counted hits). Extend the history until n ≥ 3,000; with 0 events, that puts the 95% upper bound below 0.1%. Re-learn d only for targets that missed.
- Number: mismatches among counted hits. Report n, hit rate, per-commit failure counts, and the fraction of commits with any failure.
- Rule: 0 mismatches with n ≥ 3,000 means learned slices survived this workload (a self-learned dependency set). Any mismatch means learned slices are unsound under -O2; report the rate, a commit-clustered bound, and the causes. The causes are the finding.

## E11: Measured executable compression (payload gate)
- Background: E9's fixed-width bound was 1.97×. The verifier estimated about 1.22× of xz(.text+.rela.text) for this variant: target offsets in jumps, an entry table only for functions, canonical Huffman operands per slot, tables stored as code lengths.
- Question: is the measured payload (symbols, rules, operands, Huffman tables, skeleton table, function entry table, alignment, metadata) ≤1.25× of **both** xz -9e(stage2 .text + .rela.text) = 41,212 B **and** xz -9e(linked .text) = 40,400 B? Symbol-table information is counted equally on both sides. The interpreter is not included; this is a payload gate.
- Checks: full round trip. Decode from **every** branch target and every post-call resume point, reached through the encoded jump offsets (not a side table), from serialized state alone, with offset widths iterated to a fixed point.
- Reuse e9.py by exec-ing it, as verify/extra.py does. Add only a bit writer, canonical Huffman and a decoder.
- Rule: ≤1.25× against both means building and timing an interpreter comes next. Otherwise, this encoding loses to entropy coding; that rejects this encoding, not executable compression in general.

## E12 (follows E11's pass): Run the compressed code (assumption: code must be expanded to native form to run at useful speed)
- Question: how much slower is stage2 chibicc when executed directly from `payload.bin` by an interpreter that never materializes expanded instructions, compared with native stage2, when compiling one fixed input (chibicc's `tokenize.c`)?
- Interpreter: C, a virtual register file plus the real process memory for data, stack and heap. One handler per skeleton (137). Rules run as call-threaded subroutines. Calls to external libc symbols go through a native trampoline (SysV ABI, including `%al` for varargs). Data sections are loaded natively, as the linker would.
- Correctness: the interpreted compiler's `.s` output equals native stage2's byte for byte.
- Number: interpreted / native wall time (median of 5). Also report resident code bytes (payload plus interpreter `.text`) against native `.text`, and instructions retired (perf stat).
- Rule: ≤10× means keeping cold code compressed and executable is practical (only hot code would need native form), so the next step is a hot/cold split. Above 10×, executable compression is a storage format, not an execution format, at this design.
