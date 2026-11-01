# E15 results: whole-unit output churn of E13 units (lua/lua history)

Plan: `journal/cycle7-plan.md`, E15 (ceiling for E16; no decision rule). Everything under **Measured** is produced by `run.sh` on this VM
(clang 18, 8 cores, about 5 minutes once E10's data exists). Raw outputs are in `results/`. Lines marked *interpretation* are not measurements.

## Setup
- Data: E10's preprocessed history, reused unchanged (`../e10-hidden-deps/work`: `tok.tsv` plus the gzipped `.i` CAS). That is the 1500 newest
  first-parent lua/lua commits touching core `.c/.h`, pinned at 8cb93bad, with E10's flags (`-std=c99 -O2 -fno-stack-protector -fno-common -fno-pic`).
  `run.sh` runs E10's `run.sh` first if the data is absent.
- Starting point: the ideator's `cut2.py` and `attr2.py`, kept unchanged as `ideator_cut2.py` and `ideator_attr2.py`. `e15.py` is the fixed version:
  - it hashes the **whole unit's** normalized output, i.e. the entire `.s` of the unit: every function (including non-inlined reached statics), every
    data block, string literals, and `.bss/.zero` sizes. Debug, `.ident` and `.addrsig` lines are dropped, private labels are renamed bijectively, and
    whitespace is collapsed outside strings (E8's comparator rules);
  - it compiles each distinct slice once, memoized by slice hash (37,806 slices are needed);
  - it covers all transitions, not a sample.
- Unit (E13's extraction rule): an extern target, with every other target's body replaced by its prototype. The compiled text is
  `tu.build(target + reached non-target bodies)`, the "slice". Three fixes were made to match E13 exactly:
  1. **Targets** are E13's global T symbols. Excluded are glibc's extern-inline definitions in the preprocessed TU (`atoi`, `getchar`, `tolower`, …),
     plus functions declared `static` by an earlier prototype (`getobjname` in some commits). Before this fix those accounted for 262k of the
     739,505 function-transitions, which is E14's population.
  2. **Reach** follows file-scope variable initializers (E13's `tufree.analyze`). Without this, a `luaopen_*` slice lacks the static functions
     named in its `luaL_Reg` tables.
  3. **Reach** keeps reached glibc inline definitions (`tolower`), because E13 removes only other *targets'* bodies.
- Check (`results/check.txt`): 300 sampled (TU, target) pairs on changed-TU transitions. The real E13 unit text (whole TU, other targets as
  prototypes) and the slice give identical normalized output for everything reachable from the target in **297/297** compiled pairs; 2 pairs are
  compile failures. In 1 of 297 pairs the slice `.s` has 3 blocks not reachable from the target symbol.
- Population: every (commit transition N→N+1, core TU, target present at both ends) where the TU preprocessed and split at both ends, as in E14.
- Compile failures: 80 slice-changed transitions are excluded. These are commits that do not compile with `-std=c99`, e.g. undeclared `popen`,
  `_longjmp`, `lua_numbertointeger`, or conflicting prototypes.

## Measured (`results/summary.txt`)
477,399 function-transitions; 105,530 of them on transitions where the TU's tokens changed.

| measure | count | rate / all | rate / TU changed |
|---|---:|---:|---:|
| slice key changed | 37,692 | 0.0790 | 0.357 |
| closure bodies changed (target + reached bodies) | 7,477 | 0.0157 | 0.071 |
| target body changed | 3,596 | 0.0075 | 0.034 |
| **whole-unit output changed** | **7,017** | **0.0147** | **0.066** |
| **asm_changed_ctx_only** (output changed, closure bodies identical) | **2,509** | 0.0053 | 0.024 |
| closure bodies changed, output identical | 2,912 | 0.0061 | 0.028 |

- 81.3% of slice-key changes (30,595 / 37,612) leave the whole-unit output identical.
- Slice-key rate / output rate = **5.37×**; closure-body rate / output rate = 1.07×.
- 35.8% of output changes are context-only (2,509 / 7,017), in 139 distinct commits. **This is what a projection key must explain.**
  The ideator's 40-transition pilot found 24/59 (41%).
- The decls whose text differs in the context-only cases (`results/attr.txt`; count of cases touching each): `global_State` 854, `CallInfo` 529,
  `lua_State` 436, `StackValue` 336, the `OpCode` enum (several versions, about 440 in total), `Proto` 137, `lua_Debug` 120, `l_obj/l_mem/lu_mem` 84,
  `FuncState` 79, `TKey` 78, `TValue` 77, the `TMS` enum 73.

Comparison with the ideator's 120-transition sanity check (per function on changed TUs):
- slice key 31.8% → 35.7%
- asm 5.7% → 6.6% (now the whole unit, not the target only)
- body 5.6% → 7.1% (closure, not target)

## Interpretation
- *Interpretation:* the output ceiling (0.0147 per transition, a mean lifetime of about 68 commits) sits close to the closure-body floor
  (0.0157). A key that knew exactly what the codegen depends on would change about 5× less often than the E8 slice key.
- *Side finding, measured:* E14's population was inflated by glibc extern-inline functions (getchar, tolower, …), and its "E8 minimal slice"
  missed statics reached through `luaL_Reg` tables. E14's absolute rates are diluted: its slice rate was 0.0505, against 0.0790 here on the
  corrected population. Its ratios are probably less affected, but E14 was not re-run.
