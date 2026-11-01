# E16 results: layout-projection key (lua/lua history)

Plan: `journal/cycle7-plan.md`, E16. Everything under **Measured** is produced by `run.sh` on this VM (clang 18, 8 cores, 11m44s wall).
Raw outputs are in `results/`. Lines marked *estimate* or *interpretation* are not measurements.
This is the third version of the key: v1 had a typedef bug, and v2 lacked the type probe. Both are described below, and their summaries are kept in
`results/summary-v1-typedef-bug.txt` and `results/summary-v2-no-probe.txt`.

## Setup
- Units, transitions and the output ground truth are E15's (`../e15-output-churn`): the whole-unit normalized `.s` of each E13 unit.
  There are 37,692 slice-changed function-transitions over 1500 commits; 80 are excluded as compile failures.
- Key (`e16.py`, class `Proj`). It comes from clang runs on the unit text with `-fsyntax-only`, i.e. parse and Sema with no codegen:
  `-ast-dump=json`, `-fdump-record-layouts-complete`, and one type probe. The key hashes the following over the target and every reached body:
  - **BODY:** exact preprocessed body tokens.
  - **N:** every AST node's kind, opcode, cast kind, literal value, value category and resolved type. Typedefs are expanded recursively, and
    anonymous records are named by their path, so no source locations enter the key.
  - **M:** each accessed field's bit offset, bit width and resolved type, plus the containing record's size and alignment.
  - **L:** the whole-record layout wherever a record is used by value (copy, init, argument or return, local) or is the pointee in
    `+ - += -= ++ --` or a subscript.
  - **SZ:** the `sizeof`/`alignof` argument type.
  - **TY (added in v3):** `sizeof`, `_Alignof` and `__alignof__` **as clang evaluates them** for every type the walk mentions. This catches typedef
    attributes and packed enums, and gives the evaluated value of every `sizeof`/`alignof` operand.
    - How: a syntax-only probe of the unit plus one `typedef char __e16p_i[sizeof(T)][_Alignof(T)][__alignof__(T)];` per type, read back from
      `-ast-dump-filter`.
    - Probes clang rejects (function types, `<builtin fn type>`, the implicit `__va_list_tag`) are dropped.
    - Anonymous records can't be spelled. They are covered by their parent's layout.
  - **E:** the value of each enum constant used. **ET:** each used enum type's sign and bit range.
  - **F:** every declaration of each callee and each unit function: type, storage class, `inline`, and all attributes.
  - **G:** each used global's type, storage class and attributes. For const globals and for globals defined in the unit, it also hashes the whole
    declaration and walks the initializer.
    - *Deviation:* the plan covers const globals only. A global defined in the unit is emitted into its `.o`, so it is included too.
  - **CC:** compiler, flags and target, pinned.
- Fallbacks:
  - An AST node outside the supported set falls back to the whole declaration or layout. Only `OffsetOfExpr` occurs, and it falls back to the record's
    whole layout.
  - An unresolvable field, record or enum falls back to its declaration text.
- Memoization: projection keys are memoized by slice hash.
- Run order: a 300-transition cost pilot first, then all transitions.

## Measured
**Mutation tests (`results/mutate.txt`): 19/19 pass** (the bitfield access-unit test was added in v4).
- Each of these changes the key:
  - synthetic edits:
    - the offset of a used field (size unchanged)
    - a typedef's width or signedness
    - a callee attribute (+noreturn, const→pure)
    - a used enum value
    - the pointee layout under pointer arithmetic
    - a typedef'd record local
    - pointer subtraction through a typedef'd pointer
    - **packed enum with `sizeof(E)`**
    - **`typedef int T __attribute__((aligned(32)))` on a local whose address escapes**
  - edits to real slices:
    - `lua_gc`: swapping the used `totalbytes` and `GCdebt`
    - `lua_gc`: `lu_byte` changed from char to short
    - `lua_gc`: +noreturn on `luaE_setdebt`
    - **`luaL_gsub`: `} luaL_Buffer __attribute__((aligned(64)));`**
- Three controls leave the key unchanged.
- The three tests in bold fail against v2, which lacks the type probe; this was checked by disabling the probe.

**Cost:**
- Pilot: 592 slices, 90 ms CPU each.
- Full run: 37,214 slices in 587 s wall on 8 cores. Extraction CPU (3 clang runs plus Python) per slice: mean 108 ms, median 71 ms, p90 135 ms.
- Fallbacks: 5.12% of slices, all `offsetof`.

**Full history (`results/summary.txt`):**

| | count | rate per all 477,399 function-transitions |
|---|---:|---:|
| slice key changed | 37,692 | 0.0790 |
| E15 whole-unit output changed | 7,017 | 0.0147 |
| **projection key changed** | **14,841** | **0.0311** |
| projection changed, output same (over-invalidation) | 7,824 | |

- **Ratio, projection rate / output rate: 2.12.** For comparison: v2 2.06, slice key 5.36, pilot 2.20.
- **Informative hits** (slice changed, projection equal): **22,771** in 203 distinct commits. That meets the ≥9,000 target.
- **False hits** (informative hit whose whole-unit output changed): **0**.
- 95% upper bound on the false-hit rate:
  - per hit (3/22,771): **0.013%**, which is below E10's 0.035%;
  - commit-clustered, assuming the hits within a commit are fully correlated (n_eff = 203): 1.48%.

**What over-invalidates** (`results/explain.txt`): 400 sampled transitions where the projection changed but the output did not. For each item kind,
the number of transitions where it differed, and how many of those it was the only difference in:

| item | differs | only difference |
|---|---:|---:|
| M (field access) | 229 | 93 |
| BODY/N | 171 | 80 |
| TY | 170 | 26 |
| L (pointer arithmetic) | 65 | 20 |
| F | 28 | 1 |

- M changes come mainly from the containing record's size: `global_State` 89, `GCUnion` 61, `lua_State` 60.
- TY changes come mainly from `sizeof(struct lua_State)`. The probe also sizes types that are reached only through typedef expansion behind a
  pointer, which is conservative.

## Adversarial edits (verifier) and the fix
These are hand edits to real slices. None of them occurs in the history. The table gives the key's behaviour in v2 and in v3:

| edit | output changes | v2 key | v3 key |
|---|---|---|---|
| `volatile` on used field `GCdebt` (lua_gc) | yes | caught | caught |
| enum BinOpr values reordered (luaK_posfix) | yes | caught | caught |
| const global `dummynode_` initializer (luaH_resize) | yes | caught | caught |
| callee extern → static inline (luaK_posfix) | yes | caught | caught |
| packed BinOpr/UnOpr enum (luaK_posfix, luaY_parser) | yes | caught | caught |
| typedef `lua_Integer` aligned(64), escaping local (luaO_str2num) | yes | caught | caught |
| **aligned(64) on the `luaL_Buffer` typedef only (luaL_gsub)** | yes (`andq $-64,%rsp`) | **missed** | caught |
| **synthetic `typedef int T __attribute__((aligned(32)))`** | yes | **missed** | caught |
| **synthetic packed enum, `sizeof(E)`** | yes | **missed** | caught |
| nonnull on a callee parameter; `may_alias` typedef | no | unchanged | unchanged |
| packed `global_State` | no | unchanged | changed (over-invalidation) |

- Cause of the misses: typedef expansion dropped typedef attributes, and SZ recorded only record sizes.
- Fix: TY, described in Setup.
- History outcome: the fix changed no false-hit count (0 before and after). It added about 400 projection changes, i.e. 2.06 → 2.12.

## Holes found by adversarial verification
| round | hole | effect | fix (mutation test that fails on the previous code) |
|---|---|---|---|
| 1 (v1, also seen in history: 19 false hits) | typedef expansion hid records and pointers from the by-value and pointer-arithmetic tests | `luaL_Buffer b` frame size, `StkId` subtraction | test resolved types without typedef brackets (typedef'd record local; typedef'd-pointer subtraction) |
| 2 (v2, hand edits only) | typedef attributes dropped by expansion; SZ recorded only record sizes | `aligned(64)` typedef, packed-enum `sizeof` | TY: clang-evaluated sizeof/alignof of every type used (3 tests) |
| 3 (v3, hand edits only) | bitfield access unit not captured: in `struct S{unsigned a:3,b:5,c:4;}`, `p->b=7` is `movw`, but widening `c` to 20 bits makes it `movl` with the key unchanged | load/store width | any bitfield access adds the containing record's whole layout (bitfield store test) |

After the v4 bitfield fix, the full rerun gives counts identical to v3: no Lua slice accesses a bitfield, and the `bitfield_whole_layout` event never fires.

Lesson: the key has to model the compiler's **lowering decisions** (frame alignment, access widths, enum storage size), not just C semantics. Each round was a decision the C-level view of "what the function uses" left out. Zero false hits in history did not reveal rounds 2 and 3.

## Earlier bug (v1), found by the experiment
- The first run had **19 false hits in 7 commits**.
- Cause: typedef-wrapped types failed the by-value test and the pointer test. Affected were `luaL_Buffer b` (its `initb` size moved the frame) and
  `StkId` subtraction (`sizeof(StackValue)` changed from 48 to 32).
- The fix is covered by mutation tests that fail on v1.

## Decision (plan rule)
- **0 false hits, ratio 2.12.** That is between 1.5 and 3, so neither outcome of the rule applies.
- What a function uses gave no observed false hit over 22,771 informative hits. It needs no codegen and costs about 0.1 s of parse and Sema per unit.
- It changes about 2× as often as the output, and 2.5× less often than the E8 slice key.
- Limits on that result:
  - Zero failures is evidence on this workload, not proof of soundness.
  - The commit-clustered bound (1.48%) is weak.
  - The verifier's hand edits found two soundness holes that never occur in history. Both are fixed, but they show the key's soundness rests on the
    extractor's completeness.
- *Estimate (not measured):* dropping the containing-record size from M, and probing only types used by value, would remove most of the
  "M only" and "TY only" over-invalidations (about 30% of the sample). That might bring the ratio toward 1.5. It would need its own mutation tests
  and a fresh false-hit run.
