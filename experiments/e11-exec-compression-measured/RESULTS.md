# E11: Measured executable compression (payload gate)

Reproduce: `./run.sh` (about 6 s wall). It runs E9's `run.sh`, which builds chibicc 90d1f7f stage1/stage2 and the stage2 asm. It then recomputes both denominators into `work/denominators.txt` and fails if their pinned sizes or hashes drift. Finally `e11.py` writes `work/payload.bin`, decodes it back from disk and checks it. Output is in `summary.txt`.

**Result (measured): the payload file is 47,034 B. That is 1.141× xz(.text+.rela.text) and 1.164× xz(linked .text), so it is ≤1.25× against both denominators and all checks pass. Per the rule, building and timing an interpreter comes next.** See the symbol-table caveat below.

## Encoding (what `payload.bin` contains)
`e11.py` execs E9's `e9.py`, as E9's `verify/extra.py` does, to get the same 79,400 instructions, 7,856 basic blocks and 137 skeletons. It adds a bit writer, canonical Huffman and a decoder.
- **Grammar:** E9's Re-Pair over skeleton ids, run within blocks (variant B). It stops at 887 of 1,546 rules, the point that minimises symbol cost; this is the same stop as E9. Symbols are fixed width at 10 b.
- **Stream:** block by block, each top-level symbol is followed by the operands of every instruction it expands to. Any symbol boundary is therefore a valid place to start decoding.
- **Jumps:** each jump operand stores the target's absolute bit address in the stream, with width W. W starts at 1 and is raised until every address fits: 1 → 18 → 19, and 19 is the fixed point.
- **Entry table:** covers the 324 functions only, W bits each.
- **Other operands:** every (skeleton, operand index) slot that is not a jump has its own canonical Huffman code. A slot's table stores its distinct values, sorted and Elias-gamma delta-coded, plus one fixed-width code length per value. Symbol operands are coded as indices into the symbol index space: functions first, so a function's index is its entry-table index.
- **Skeleton table:** mnemonic and operand-template strings, NUL-terminated, plus per-skeleton indices.
- **Metadata:** eight 32-bit header fields.
- **Alignment:** padding before the stream and at the end.

## Payload (measured: size = `len(payload.bin)`, sha256 `8fa82444b07041ab…`)
| component | bytes |
|---|---|
| metadata (header fields) | 32.0 |
| skeleton table | 1,116.9 |
| Re-Pair rule bodies (887 × 2 × 10 b) | 2,217.5 |
| Huffman tables (values + code lengths) | 2,217.4 |
| function entry table (324 × 19 b) | 769.5 |
| top-level symbols (10,995 × 10 b) | 13,743.8 |
| operands, Huffman (non-jump) | 16,877.2 |
| jump target offsets (4,235 × 19 b) | 10,058.1 |
| alignment | 1.6 |
| **total = file length** | **47,034** |

| denominator (recomputed here, measured) | input | xz -9e | ratio |
|---|---|---|---|
| stage2 9 × `.o` `.text`, then 9 × `.rela.text` (sha256 `9aa5a610f39cd053…`, 327,546 B) | E9 verify's concatenation | 41,212 | **1.141** |
| linked `build/stage2/chibicc` `.text` (sha256 `e7e0c2b0ff2de72b…`, 263,634 B) | E9's file | 40,400 | **1.164** |

The E9 verifier's estimate for this variant was about 50.2 KB (1.22×). The measured file is smaller for two reasons:
- The Huffman tables cost 2.2 KB as delta-coded values, not the estimated 5.9 KB at raw width plus 5 b per value.
- That outweighs the skeleton table, which is 1.1 KB measured against a 0.55 KB estimate.

The operand and jump bytes match the estimate: 16,877 B and 10,058 B.

## Symbol-table information (how it is counted equally)
- **Per-reference symbol identity is counted on all three sides.** D1 carries a `.rela.text` entry with a symbol index. D2 carries the resolved address in the instruction bytes. The payload carries a Huffman-coded symbol index.
- **Symbol definitions are counted on no side.** Neither denominator includes `.symtab`/`.strtab`; D2 also leaves out the data sections, whose layout fixes the addresses. The payload leaves out the index → name/address list. The checker uses that list only to compare decoded symbol indices with the original names.
- **The function entry table is counted in the payload** even though it duplicates function-symbol information, which is conservative.
- **Caveat (estimate, not measured):** suppose an interpreter also had to carry an address for each of the 1,323 non-.text symbols, at E9's 32 b each (5,292 B), with nothing added to the denominators. The payload would be 52,326 B: 1.270× / 1.295×, which is above 1.25×. The gate passes under the equal-counting rule stated in the plan. It does not pass if only our side is charged for the symbol table.

## Checks (measured; all from a decoder built from the bytes of `payload.bin` read back from disk)
- **Full round trip: OK.** A sequential decode from bit 0 yields all 79,400 instructions. Skeletons, immediates and displacements match exactly. Symbol indices map to the original names. Every jump operand equals the bit address of its original label's block.
- **Every branch target: 2,523 / 2,523 OK.** The 4,235 jumps name 3,395 labels, which fall on 2,523 distinct addresses. Each address was reached by reading a decoded jump's offset field. Decoding starts fresh at that address and runs to the first terminator (`j*`, `call`, `ret`). It matches the original instructions from that label's block on, through any fall-through blocks.
- **Every post-call resume point: 2,478 / 2,478 OK.** Each resume point is the decoder's position right after a decoded `call`; decoding from there matches the original next block.
- **Every function entry, via the entry table: 324 / 324 OK.**
- Together these reach 5,325 of the 7,856 block starts. The other 2,531 blocks are entered only by fall-through, and their starts are not separately addressable, by design. No reached address falls anywhere other than a block start.
- **Negative control (done once by hand, not in `run.sh`):** flipping one bit in the stream makes the round trip and a resume-point check fail.

## Scope
- This is a payload gate only. The interpreter, and its handler size and speed, are not measured.
- Re-Pair is E9's greedy run. The grammar symbols are fixed width; Huffman-coding them was not tried.
