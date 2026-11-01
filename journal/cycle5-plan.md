# Cycle 5 plan

Assumption shared by every experiment so far (ideator, 2026-11-01): "correct" means bit-identical to what one compiler emits when it sees the whole TU. That forces each mechanism to copy the compiler's global quirks (E7's label counter, E8's last-call bonus). Ideator spot-checks: per-TU flag search beats -Oz by only 1.0%. A zstd dictionary trained on other programs shrinks Lua's .text by ≤1.1%. Across Lua 5.4.6→5.4.7, 572/644 functions are unchanged after normalization.

## E13: TU-free build (assumption: a compiler needs the whole TU to emit good code)
- Question: if Lua 5.4.6 is built so that each externally visible function is compiled alone from a local slice (no worker sees a whole TU), does the result pass Lua's official test suite, and how much slower is it than a normal per-TU `clang -O2` build?
- Slice per extern target: the target's body, the referenced top-level declarations, the bodies of **static** callees transitively (they are private to the TU, so a worker must carry them; they compile as local copies), and prototypes only for extern callees. One object per target; only the target symbol is exported.
- File-scope static **data** must stay a single instance. Rewrite it to a hidden-visibility extern with a TU-unique name, defined once in a per-TU data object. Read-only data may be duplicated.
- Inputs: Lua 5.4.6 plus the official `lua-5.4.6-tests` (basic mode, `_U=true` as the tests' README describes). Benchmark: a fixed set of 5 standard Lua programs (fib, binary-trees, spectral-norm, n-body, string ops), median of 5 runs each.
- Baselines: per-TU `clang -O2`, and ThinLTO `-flto=thin -O2`, both as context.
- Number: geometric-mean runtime ratio of the TU-free build to per-TU -O2. Also report the median slice size and total compile CPU.
- Rule: tests pass and ratio ≤1.03 means bit-identity was too strict a bar; TU-free compilation is viable, and the slice keys from E8 become a practical build unit. A failing test, or ratio >1.10, means global TU state is load-bearing; record which. Between 1.03 and 1.10: viable at a measured cost, recorded as such.
