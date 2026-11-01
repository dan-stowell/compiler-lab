# Cycle 3 plan

E5 found that under chibicc a function compiles to identical asm from its body, a ~2.5% context slice, and one "defined here" bit per callee. E7 found that output hashes are stable once global counters are normalized away. An architecture challenge (adversary, 2026-11-01) argued that slice-keyed caching on chibicc just re-derives Bazel, rustc queries and ThinLTO, and that the first E8 design (a slice memo over history) would pass by construction. The one possibly new claim is that a tiny, fixed amount of callee information is enough. That holds for a non-optimizing compiler; whether it survives optimization is the decisive test.

## E8: Do slices survive -O2? (assumption: optimized code of a function depends on its callees' bodies)
- Input: Lua 5.4.6 `lparser.c`, preprocessed with the project's own flags. Compiler: clang -O2 (installed version, recorded).
- Question: for each function the full-TU compile emits, what fraction needs callee **bodies** in its slice to reproduce its normalized asm byte for byte? Try the information levels in order, stopping at the first that matches:
  1. callee prototypes only
  2. prototypes with a "defined here" bit (`static` and visibility preserved; body-less `noinline` stubs are not allowed, since they change output)
  3. callee bodies, transitively
- Functions absent from the full-TU output (inlined everywhere) are reported separately, not in the fraction. Normalize by renumbering local labels (.LBB, .Ltmp, .LCPI and friends) and stripping debug and section directives. Compare only the function's own text plus the constant-pool data it references.
- Number: the fraction of emitted functions that need level 3.
- Rule: ≤10% means a small callee summary survives optimization, which is worth building on. Above 10% means the direction reduces to ThinLTO plus Bazel; stop it and record why.
