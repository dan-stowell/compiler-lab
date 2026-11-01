# E16 results: layout-projection key (lua/lua history)

Plan: `journal/cycle7-plan.md`, E16. Everything under **Measured** is produced by `run.sh` on this VM (clang 18, 8 cores, 8m51s wall).
Raw outputs are in `results/`. Lines marked *estimate* or *interpretation* are not measurements.

## Setup
- Units, transitions and the output ground truth are E15's (`../e15-output-churn`): the whole-unit normalized `.s` of each E13 unit.
  There are 37,692 slice-changed function-transitions over 1500 commits; 80 are excluded as compile failures.
- Key (`e16.py`, class `Proj`). It is built from `clang -fsyntax-only -Xclang -ast-dump=json` and `-Xclang -fdump-record-layouts-complete`
  on the unit text: parse and Sema, no codegen. The key is a hash over the target and every reached body:
  - **BODY:** exact preprocessed body tokens.
  - **N:** every AST node's kind, opcode, cast kind, literal value, value category and resolved type. Typedefs are expanded recursively, and
    anonymous records are named by their path, so source locations don't leak into the key.
  - **M:** for each accessed field: bit offset, bit width, resolved type, and the containing record's size and alignment.
  - **L:** whole-record layout, wherever a record is used by value (copy, init, argument or return, local) or is the pointee in `+ - += -= ++ --`
    or a subscript.
  - **SZ:** `sizeof`/`alignof` argument type, plus the sizes of the records it contains.
  - **E:** values of the enum constants used. **ET:** the sign and bit range of each enum type used.
  - **F:** every declaration of each callee and each unit function: type, storage class, `inline`, all attributes.
  - **G:** used globals: type, storage class, attributes. For const globals, and for globals defined in the unit, the whole declaration plus a walk of
    its initializer.
    *Deviation:* the plan says const globals only. A global defined in the unit is emitted into the unit's `.o`, so it is included too.
  - **CC:** compiler, flags and target, pinned.
- Fallbacks:
  - An AST node outside the supported set falls back to the whole declaration or layout of the records involved. Here only `OffsetOfExpr`
    occurs, which falls back to the record's whole layout.
  - An unresolvable field, record or enum falls back to its declaration text.
- Memoization: projection keys are memoized by slice hash, like E15's compiles.
- Run order: first a 300-transition cost pilot (seeded order), then all transitions.

## Measured
**Mutation tests (`results/mutate.txt`): 15/15 pass.**
- Each of these changes the key:
  - synthetic: the offset of a used field (size unchanged); a typedef's width; a typedef's signedness; a callee attribute (+noreturn,
    const→pure); a used enum value; the pointee layout under pointer arithmetic; a typedef'd record local used by value; pointer subtraction
    through a typedef'd pointer;
  - on a real `lua_gc` slice: swapping the used `global_State.totalbytes` with `GCdebt`; `lu_byte` from char to short; +noreturn on the callee
    `luaE_setdebt`.
- Controls that leave the key unchanged: a same-size type change of an unused field; an attribute on an unrelated function; swapping two unused
  `global_State` fields.

**Cost:**
- Pilot: 592 slices in 6 s wall, 65 ms CPU per slice.
- Full run: 37,214 slices in 445 s wall on 8 cores. Extraction CPU (2 clang runs plus Python) per slice: mean 82 ms, median 49 ms, p90 93 ms.
- Fallbacks: 5.12% of slices (1,937), all `offsetof`.

**Full history (`results/summary.txt`):**

| | count | rate per all 477,399 function-transitions |
|---|---:|---:|
| slice key changed | 37,692 | 0.0790 |
| E15 whole-unit output changed | 7,017 | 0.0147 |
| **projection key changed** | **14,433** | **0.0302** |
| projection changed, output same (over-invalidation) | 7,416 | |

- **Ratio, projection / output change rate: 2.06** (slice / output: 5.36). The 300-transition pilot gave 2.10.
- **Informative hits** (slice changed, projection equal): **23,179**, in 206 distinct commits. The ≥9,000 target is met.
- **False hits** (informative hit whose whole-unit output changed): **0**.
- 95% upper bound on the false-hit rate:
  - per hit (rule of three, 3/23,179): **0.013%**, below E10's 0.035%;
  - commit-clustered, with hits in a commit fully correlated (n_eff = 206 commits): 1.46%.

  The true clustering lies between the two. Only the per-hit bound is below 0.035%.

**What over-invalidates** (`results/explain.txt`: 400 sampled transitions where the projection changed and the output did not; the number of
transitions with that kind of difference, and in how many it was the only difference):

| differing item | transitions | the only difference |
|---|---:|---:|
| field access (M) | 241 | 166 |
| bodies or node types | 184 | 87 |
| layout under pointer arithmetic | 69 | 22 |
| callee prototype (F) | 33 | 1 |
| layout used by value | 23 | 7 |

- The records behind M differences: `global_State` 94, `GCUnion` 66, `lua_State` 61, `CallInfo` 16.
- *Interpretation:* these are mostly the containing-record size and alignment that the plan puts into M (`global_State` grows, while the used fields'
  offsets stay put), and `GCUnion`, whose size follows the largest GC type.
- Body differences that leave the output unchanged are E15's 2,912 "closure bodies changed, output identical" cases (2,912 of the 7,416
  over-invalidations). No key that includes the bodies can avoid them.

**Extractor bug found by the experiment itself:**
- The first full run (`results/summary-v1-typedef-bug.txt`) had **19 false hits in 7 commits** (0.080%, clustered CI 0.023–0.155%).
- Cause: in the extractor, a typedef-expanded type (`[struct luaL_Buffer]`, `[[union StackValue] *]`) failed two tests. The by-value test missed
  locals such as `luaL_Buffer b`, whose `initb` size moved the stack frame. The pointer test missed pointer subtraction on `StkId`, whose
  `sizeof(StackValue)` changed from 48 to 32.
- Both cases are now mutation tests, and both fail against the old code. The same bug had also kept the ET items out of the key.
- After the fix: 0 false hits.

## Decision (plan rule)
- **0 false hits, ratio 2.06.** That is above 1.5 and below 3, so neither outcome of the rule applies.
- "What a function uses" gave **no observed false hit** over 23,179 informative hits on this workload. It needs no codegen, at about 50 ms
  of parse and Sema per unit.
- It still changes about 2× as often as the output. It isn't "about as rarely" (≤1.5×), and it doesn't over-invalidate badly (>3×).
- It changes **2.6× less often than the E8 slice key** (0.030 vs 0.079).
- Zero failures is evidence on this workload, not proof of soundness. The commit-clustered bound (1.46%) is far weaker than the per-hit
  bound, because the hits come from only 206 commits.
- *Estimate (not measured):* dropping the containing-record size from M where only the field offset matters would remove most of the 166
  "M only" over-invalidations (about 40% of the sample), and might bring the ratio near 1.5. It would need its own mutation tests and its own
  false-hit run.
