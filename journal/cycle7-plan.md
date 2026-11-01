# Cycle 7 plan

Ideator sanity check (single sample, unverified): over 120 random changed-TU transitions (2,179 extern functions as E13 units), the minimal slice key changed for 31.8% of functions, the normalized -O2 asm for 5.7%, and the body for 5.6%. 82% of key changes leave the asm identical. Struct definitions drive context churn: `global_State` alone causes 40% of context-only key changes, followed by `Table`, `CallInfo`, `TValue` and `l_obj`.

## E15: Output churn of E13 units (ceiling only, revised after review)
- Output-keyed caching is early cutoff (rustc, Shake), not a new mechanism. E15 exists only as E16's ceiling and has no decision rule.
- Measure: over E10's 1500 commits, the per-transition change rate of the **whole unit's** normalized output (every function and data block in the unit's .o, including non-inlined reached statics and bss sizes), for each extern function compiled as an E13 unit (E13's extraction rule exactly).
- Reuse the ideator's `cut2.py`: hash the whole unit, memoize compiles by slice hash, and run all transitions. Report `asm_changed_ctx_only` (output changed, closure bodies unchanged). That is what a projection must explain; a 40-transition pilot found 24 of 59 output changes.

## E16: Layout-projection key (assumption: a function depends on whole declarations)
- Question: does a key computed without codegen, from what the unit actually uses, change about as rarely as E15's output with no observed false hits?
- Key, as a union over the target and every reached static:
  - the exact preprocessed bodies
  - resolved, desugared types of every expression and local (typedef widths, signedness, qualifiers)
  - each accessed field's bit offset and width, type and qualifiers, plus the containing record's size and alignment
  - whole-record layout wherever a record is copied, initialized, passed or returned by value, or used in pointer arithmetic
  - `sizeof`/`alignof`/`offsetof` values
  - enum constants used
  - callee prototypes **with attributes** (noreturn, pure, inline and so on)
  - global variables' types, plus initializers of const globals used
  - the target's signature, linkage and visibility
  - compiler, flags and target pinned
- Unsupported AST nodes fall back to hashing the whole declaration; report the fallback frequency and the extraction CPU.
- Extraction: clang `-ast-dump=json` filtered to the unit's FunctionDecls (resolve `MemberExpr` through its referenced decl, keep desugared types), with layouts from `-fdump-record-layouts`. Use a LibTooling visitor only if JSON proves insufficient.
- Counting: an informative hit is one where the slice key changed but the projection key is equal; aim for ≥9,000 (needed to bound below E10's 0.035%). A false hit is an informative hit whose E15 whole-unit output changed. Report distinct commits and exclusions. Add mutation tests: change a used field's offset, a typedef width or a callee attribute, and confirm each changes the key.
- Number: projection change rate / E15 output change rate, and false hits / informative hits, with a commit-clustered bound.
- Rule: ratio ≤1.5 with 0 false hits means "what a function uses" is an empirically sound key that needs no codegen, a dependency mechanism finer than declarations. Any false hit is the finding; report its cause. Ratio >3 means the projection over-invalidates.
- Wording: zero failures is evidence on this workload, not proof of soundness. Extraction still parses and type-checks; it only skips codegen.
