# E9: Executable compression, step 1 (the decider)

Reproduce: `./run.sh` (about 4 s wall). It clones chibicc into `./chibicc` and builds 90d1f7f with gcc into `./build` (stage1). It builds `build/stage2/chibicc` (stage1 compiling chibicc) and a clang `-Oz` build in `./build-oz`, then writes `work/` and `summary.txt`. `build*/`, `work/` and `chibicc/` are gitignored.

**Result: the lower bound is 1.97× the denominator (1.58× against the linked-binary variant), above the 1.25× rule. This representation loses to entropy coding. Stop; the full encoder is not built.**

## Setup
- Stage2 asm is `build/chibicc -S` on each of the 9 sources. Two checks pass in every run:
  - `stage2/chibicc -S` produces identical asm, apart from `.file` paths to the bundled headers.
  - `as` on that asm gives a `.text` byte-identical to the one in each `build/stage2/*.o`.
- Denominator: `xz -9e` of stage2's `.text` (measured). Primary: the 9 `.o` `.text` sections concatenated, 263,370 B, sha256 `d8602f4c4d1a7675…`. This is the plan's pinned ≈32.5 KB; relocation fields are zero. Alternative: the linked `build/stage2/chibicc` `.text`, 263,634 B, sha256 `e7e0c2b0ff2de72b…`. It has relocations resolved plus 264 B of crt code. It is larger after xz, which favours the bound, and the decision holds against both.
- Parsing (`e9.py`): only instructions inside `.text` sections. An instruction becomes a skeleton (mnemonic, registers and operand shape) plus operand values in four kinds: immediate `$I`, displacement `D(%reg)`, symbol `S(%rip)`/`S@GOTPCREL(%rip)`, and jump label `L`.
  - TU-local names are qualified by TU, so every label and symbol stays distinct. The 27 static `__va_arg_*` copies count as separate code, and the script asserts there are no label collisions.
  - This fixes the scratch `repair.py`, which collapsed every `.L..N` into one terminal.
- Basic blocks break before every label and after every `j*`, `call` and `ret`. Re-Pair runs within blocks only, so every branch target begins a top-level symbol and needs no decoding of preceding code.
- Re-Pair runs to completion, and the cost is evaluated after every rule; the reported stopping point minimises total bits. A round trip (expand the grammar, compare with the input) passes for both variants.
- Label table: B = 3,719 entries, made of the 324 functions and every `.L` label that is referenced. The 243 labels nobody references are dropped. Each entry stores the top-level symbol position and, in variant B, an offset into the operand stream. Variant A needs no operand offset, because operands live in the instruction table.
- Operand width per slot (skeleton, operand index) = ⌈log2(distinct values in that slot)⌉, as the plan specifies. This leaves out the dictionary that maps the indices back to values; the "fuller bound" row below adds it.

## Counts (measured)
79,400 instructions in 7,856 blocks; 6,066 distinct whole instructions; 137 skeletons; 55 operand slots carrying 36,454 operands; 1,628 distinct referenced symbols, 1,323 of them outside `.text` (data, string literals, libc).

## Component breakdown (bytes)
| component | A: whole-instruction symbols | B: skeleton symbols + operand stream | kind |
|---|---|---|---|
| Re-Pair rules used (of full run) | 2,126 (of 3,721) | 887 (of 1,546) | measured |
| top-level symbols | 22,936 × 13 b = 37,271 | 10,995 × 10 b = 13,744 | measured |
| rule bodies (2 symbols each) | 6,910 | 2,218 | measured |
| operands | 7,582 (in instruction table) | 32,542 (stream) | measured |
| instruction table: skeleton ids | 6,066 | none | measured |
| skeleton table (32 b/skeleton) | 548 | 548 | **estimate** |
| label table | 6,973 (B × 15 b) | 14,876 (B × (14 + 18) b) | measured |
| **payload bound** | **65,350** | **63,927** | measured, except the skeleton table |
| ratio vs 32,524 (.o .text, primary) | 2.009 | **1.966** | |
| ratio vs 40,400 (linked .text) | 1.618 | **1.582** | |

Taking the smaller, variant B: **63,927 B = 1.97×** (1.58× linked).

Further rows on the smaller variant (B):
| addition | bytes | ratio (primary / linked) | kind |
|---|---|---|---|
| + per-slot value dictionaries | 2,592 | | measured |
| + symbol address table (1,323 × 32 b) | 5,292 | | **estimate** |
| fuller bound | 71,811 | 2.21 / 1.78 | |
| handler cost, 137 skeletons × 16 B | 2,192 | payload + handlers 2.03 / 1.64 | **estimate** |
| handler cost, 137 skeletons × 32 B | 4,384 | payload + handlers 2.10 / 1.69 | **estimate** |

Context (measured): clang `-Oz` `.text` is 62,738 B, and `xz -9e` of it is 31,532 B.

## Why it loses
- In variant B, operands alone at fixed width (32.5 KB) already equal xz's size for the whole `.text`. xz exploits value skew (frame offsets like −8, −16, and hot symbols) and context; a fixed-width code cannot.
- Branch-entry addressability is expensive. 3,719 entry points × 32 bits is 14.9 KB in B. A smaller operand stream (A) moves the cost into a 13-bit symbol alphabet instead.
- Re-Pair within basic blocks is limited: the average block is 10 instructions, so whole-instruction repeats are short.

## Caveats
- Re-Pair is greedy, so this is a bound for this representation built with Re-Pair, not for every grammar. Since the payload is 1.58–1.97× the denominator, a better grammar would have to save 21–37% of the total payload before it reached 1.25×. Operands and the label table, which a better grammar does not change, already account for 74% of B's payload.
- Variable-length or entropy-coded operands could narrow the gap, but they are outside the fixed-width representation tested. As the plan says, this rejects the representation, not executable compression in general.

## Independent verification (2026-11-01)

The numbers reproduce exactly. The bound is **not tight**: the label table is inflated by the separate operand stream. These are the verifier's **estimates**, from scripts in `verify/`:
- Interleaving operands with symbols: 57.9 KB (1.78×).
- Target offsets inside the 4,235 jumps, with a table only for the 324 functions: about 54.2 KB (1.67×; 1.31× against xz of .text+.rela.text = 41,212 B).
- Adding Huffman-coded operands per slot, dictionaries counted: about 50.2 KB, i.e. 1.54× primary, 1.24× linked, **1.22× text+rela**.

The fixed-width layout loses. The best estimated variant sits at the 1.25× threshold against relocation-inclusive denominators. Undecided until it is built and measured.
