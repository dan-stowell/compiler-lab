# Cycle 6 plan

Synthesis so far:
- E13: a function plus the statics it reaches, compiled alone with other externs as prototypes, gives a correct Lua at no instruction cost.
- E10: under that unit, outputs differ from the whole-TU compile in 0.035% of cache hits. The difference is in bits, not semantics.
- So a TU-free key, meaning the exact text a worker compiles, is sound by construction: the output is a deterministic function of the key.

The open question is whether such keys live long enough to make investing spare cycles in them pay off.

## E14: Lifetime of TU-free function keys (assumption: expensive optimization can't pay for itself, because results are invalidated too fast)
- Key: the E13 worker input for an extern function, i.e. the preprocessed TU with other extern bodies as prototypes, after dead-static elimination at the source level (statics the target doesn't reach transitively are removed), token-normalized, with compiler and flags.
- Input: lua/lua first-parent history, the same commit range and file set as E10 (core files, excluding onelua/ltests/lua/luac). Reuse E10's checkout, preprocessing and failure handling.
- Number: median key lifetime in commits (the number of consecutive commits a function's key stays unchanged, censored at the end of the history), compared with the per-TU key (ccache-style hash of the preprocessed TU) for the same function.
- Also report the miss fraction per commit (TU-free vs per-TU) and the worker input bytes per commit (sum of missed keys' sizes vs sum of changed TUs' sizes).
- Rule: if TU-free key lifetime is ≥20× the per-TU lifetime, a per-function cache of expensively optimized results (superoptimization, PGO, search) can amortize over many builds, so build one next. Under 5×, stop: keys churn too fast for spare-cycle investment. In between, report it and pick by the bytes metric.
