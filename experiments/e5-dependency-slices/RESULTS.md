# E5: Dependency slices for parse.c

Reproduce: `./run.sh` (clones and builds chibicc 90d1f7f into `./chibicc`, gitignored; takes about 5 s).
Outputs: `slices-strict.csv` (prototypes only, as planned), `slices-stubs.csv` (callees as empty-bodied stubs), `summary.txt`.

## Setup
- TU: `chibicc -E parse.c`, run inside the chibicc tree. It has 29,852 tokens in 836 top-level declarations. 19,583 of those tokens are in the 116 function definitions, and 10,269 are outside them. The splitter finds 116 functions, the same number of `@function` symbols that `chibicc -S` emits for the full TU. Two of the 116 (`__va_arg_*`) come from chibicc's own `stdarg.h`.
- Splitter (`slice.py`): token-level, adapted from E2. A declaration ends at a `;` at depth 0, or at the `}` that closes a function body. A function body is a depth-0 `{` after `)` with no earlier depth-0 `=`, so the compound literal `static Scope *scope = &(Scope){}` is not taken for a body.
- Closure: identifiers in the whole function definition map to the declarations that declare them. Tags (`struct X`) and ordinary names (typedefs, enum constants, globals, prototypes) are separate namespaces, and member names after `.` or `->` are ignored. Every declaration of a name is included (all prototypes, forward declarations and definitions), iterated to a fixpoint and emitted in original order. Struct members and pointer targets are followed too, even where an incomplete type would do. Another function contributes only its prototype. If it has no prototype before the target, a prototype is made from its header.
- Size: (slice tokens + target function tokens) / TU tokens.
- Verification: every function is compiled with `chibicc -S`. The script cuts out that function's text (from its `.globl`/`.local` line to the next one), drops `.loc` and `.file` lines, and renumbers every `.L.<kind>.N` label in order of first use within the function (`.L..N` string or anonymous labels, and `.L.else/.end/...`). The result is compared with the same function cut from the full-TU asm. A failed compile or any difference counts as 100%.

## Results (measured)
| variant | verified | failed | median (failed = 100%) | p90 | raw median / max |
|---|---|---|---|---|---|
| strict: other functions as prototypes only | 23 | 93 | **100%** | 100% | 2.81% / 7.94% |
| stubs: other TU functions as `header {}` | **116** | 0 | **2.85%** | 4.14% | 2.85% / 8.39% |

- **Every strict failure has the same cause.** chibicc takes a function's address with `lea f(%rip)` when `f` is *defined* in the TU, and with `mov f@GOTPCREL(%rip)` when it is only declared (codegen.c:143). The script checked all 93: each matches the full-TU asm exactly once that one instruction form is mapped for TU-defined callees. No slice failed to compile. So a prototype does not carry everything the codegen depends on. The callee's "defined in this TU" bit is missing.
- With that bit restored by an empty body, all 116 slices verify, at a median of 2.85% of the TU and p90 4.14%. The context alone (slice minus the target's own body) has a median of 752 tokens, which is 2.52% of the TU and 7.3% of the TU's tokens outside function bodies.
- **Largest slice: `stmt`, 8.39%** (2,506 tokens). Its own body is 1,358 tokens, the largest definition in the TU, so it accounts for 54% of that. The rest is 45 declarations plus 14 stubs. The biggest of those are the chibicc.h type cluster: `struct Node` 140, the `NodeKind` enum 102, `struct Obj` 88, `struct Type` 72, `struct Token` 62. This mutually referencing cluster (Node→Obj→Type→Token→Member→Relocation…) appears in nearly every slice and sets the ~2.5% floor. System-header declarations add little (median 10 tokens per slice, max 76, mostly the `int64_t` and `size_t` typedef chains; measured).

## Decision
The rule is median ≤ 10% → viable. As the plan specified it (prototypes only), the median is **100%**: every function that takes another TU function's address fails verification, so by the letter of the rule the result is **not viable**. The cause is one bit of information per callee (defined here or not), not the header cluster. With that bit added (stubs variant), the median is **2.85% with 116/116 verified → viable**. A content-addressed slice for a stateless worker needs a function's prototype plus whether it is defined in the TU. The hypothesis that the header's mutually referencing structs would pull in too much is refuted: they cost about 750 tokens.

## Estimated / caveats
- The 7.3% figure (context against tokens outside function bodies) is arithmetic on the measured medians.
- "Verified" means only the target function's text section is compared. Data emitted for its string literals is not compared, but the labels referring to it are.
- The rule follows every identifier, so struct bodies reached only through pointers are included. A slice that stops at incomplete types would be smaller, but this was not measured.
- This covers one TU and one compiler. The definedness dependence is a chibicc codegen choice, and other compilers were not checked.
