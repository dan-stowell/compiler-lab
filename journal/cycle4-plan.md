# Cycle 4 plan

## E10: Hidden dependencies of learned slices (assumption: a cached per-function result needs a sound, explicit dependency set)
- Background: E8 found that a function's clang -O2 output is reproduced by a median 6.5% slice (callee bodies to the smallest depth d that matches). In a real system the slice would be learned once, by comparing against a full-TU compile, and then reused: the function is recompiled only when its slice's content changes. That is unsound if the full-TU output changes because of code outside the slice. E8's close_func effect shows how: a new caller of a static callee changes inlining.
- Input: Lua's git history (github.com/lua/lua), the last 200 first-parent commits that touch core `.c`/`.h` files. clang 18, `-O2 -fno-pic`, E8's slicer and comparator (with the string fix).
- Procedure: at commit N, learn each extern function's smallest matching slice (as in E8). At commit N+1, for every function whose learned slice content is unchanged (a would-be cache hit), check whether its full-TU normalized asm at N+1 equals its asm at N.
- Number: the hidden-dependency rate, i.e. would-be hits whose real output changed, as a fraction of all would-be hits, pooled over the history. Also report the hit rate.
- Rule: ≤0.1% means learned slices can be trusted, with occasional full-TU audits as a backstop, so self-learned dependency sets are a viable mechanism. Above 0.1%, learned slices are unsound under -O2; report the causes.

## E11: Build the best executable-compression variant (assumption: compact code must be decompressed before it runs)
- Background: E9's fixed-width bound was 1.97×. The verifier estimated about 1.22× of xz(.text + .rela.text) for a variant that puts target offsets in jumps, keeps an entry table only for functions, and Huffman-codes operands per slot with dictionaries counted. That is undecided against the 1.25× rule.
- Question: is the **measured** size of that encoding, every byte needed to execute included, ≤1.25× xz -9e of stage2's .text + .rela.text (41,212 B)?
- Checks: decode the whole program back to the original instruction sequence (round trip). For 100 random branch targets, decode from the target with no preceding state and match the original block.
- Rule: ≤1.25× means building the interpreter and measuring speed comes next. Otherwise, record that executability costs more than 25% over entropy coding for this code.
