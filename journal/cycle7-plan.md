# Cycle 7 plan

Ideator sanity check (single sample, unverified): over 120 random changed-TU transitions (2,179 extern functions as E13 units), the minimal slice key changed for 31.8% of functions, the normalized -O2 asm for 5.7%, and the body for 5.6%. 82% of key changes leave the asm identical. Struct definitions drive context churn: `global_State` alone causes 40% of context-only key changes, followed by `Table`, `CallInfo`, `TValue` and `l_obj`.

## E15: Output-keyed lifetime, i.e. early cutoff (ceiling for E16)
- Question: over E10's 1500 commits, what is the per-transition change rate of each extern function's normalized -O2 asm when compiled as an E13 unit (function + reached static bodies + reachable declarations)?
- Number: lifetime ratio against per-TU (rate_perTU / rate_output). E14's comparators: per-TU 0.183, slice 0.051, body 0.0049.
- Also report the cheap-compile CPU per transition.
- Rule: ≥20× per-TU means an expensive-optimization cache keyed on cheap output can amortize (pending a benefit estimate). Under 10×, stop.
- Normalization caution: E7/E8 label renaming must not hide real changes; reuse the E8 comparator with its string fix and self-test.

## E16: Layout-projection key (assumption: a function depends on whole declarations)
- Question: can a key computed **without compiling** match E15's output churn with no false hits? The key is the function body, plus the reached static bodies, plus the **projection** of the context the function actually uses:
  - for each accessed struct/union field: offset, size and type
  - each `sizeof`/`alignof` value used
  - each enum constant and macro-expanded constant used
  - callee prototypes
  - the types of globals used
- How: clang `-Xclang -ast-dump=json` (or a small libclang/clang-query script) per E13 unit gives the used fields and values, resolved to layout facts (offsets via `-fdump-record-layouts`).
- Input: the same transitions as E15. Count false hits: key equal but E15 asm different. Need N ≥ 2,000 hits for the bound.
- Number: projection-key change rate / output change rate, and the false-hit count.
- Rule: ratio ≤1.5 with 0 false hits means "what a function uses" is a sound, compile-free key: a new dependency mechanism. Any false hit is the finding; report its cause (inlining, which E10 showed matters, as well as struct passing by value or alignment). Ratio >3: the projection misses what churns.
- Scope: E15 first; E16 can start on a 300-transition subset to bound cost.
