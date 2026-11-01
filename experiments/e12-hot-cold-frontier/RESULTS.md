# E12: Hot/cold frontier for executable compression

Reproduce: `./run.sh` (about 15 s wall; needs valgrind). It runs E11's `run.sh`, which builds chibicc 90d1f7f stage1/stage2 through E9 and writes E11's `payload.bin`. It then builds and checks the C decoder, profiles stage2 and computes the frontier. Output: `summary.txt`, `profile.csv` (per-function profile), `frontier.csv` (k = 0..324).

**Result: keeping runtime at ≤1.5× native needs 72.4% of native stage2 `.text` resident under the optimistic cost and 86.9% under the pessimistic one. The optimistic case is above 50%, so the plan's rule says stop: executable compression buys little RAM for this program.** These are estimates built from measured inputs (see below). The numbers come from the `run.sh` run recorded in `summary.txt`.

Machine: AMD EPYC 9554P (exe.dev VM, 8 vCPUs), Linux 6.12.113, gcc 13.3.0.

## Measured inputs
**Profile.** Native stage2 ran `-cc1 -cc1-input tokenize.c -cc1-output t.s tokenize.c` from `build/`, under `valgrind --tool=callgrind --dump-instr=yes`. Its output equals `chibicc -S tokenize.c`. Self Ir is attributed by instruction address to the `.text` symbol range that contains it (`profile.py`).
- Total Ir is 967.0 M. The 324 chibicc functions account for 784.0 M (81.07%). libc accounts for 182.3 M (18.85%), and ld.so plus unknown objects for 0.09%.
- The chibicc/libc split is an **Ir share, not a time share**. There was no perf on the VM, so time is assumed proportional to Ir.
- The profile is very skewed. `in_range` alone is 234 M Ir, and the top 14 functions cover 90% of chibicc's Ir (the reviewer's profile said 12).

**Native sizes.** chibicc emits no `.size` directives, so `nm --size` reports nothing for its functions. Each function's size is therefore the gap from its `nm -n` address to the next `.text` symbol, padding included. The 324 functions total 263,383 B, out of 263,634 B of `.text`.

**Native wall time.** Median of 5 runs after one warm-up, measured from Python `subprocess`, so process start and exit are included: **0.0944 s**. The runs ranged from 0.078 to 0.112 s, which is noisy.

**C decoder** (`dec.c`, gcc -O2):
- **Round trip:** decoding all of `payload.bin` from bit 0 gives 79,400 records (start bit, skeleton, operand values). Their text dump is byte-identical to a dump of E11's own Python decoder output (`dump_e11.py` execs `e11.py`, checks that the rebuilt payload equals E11's file, and dumps `full`).
- **Speed:** sequential whole-payload decode, median of 1,001 repetitions: **9.66 ns per decoded guest instruction** (minimum 8.89). This is decode only: no execute, no dispatch.
- **Size:** `dec.o` `.text` (init + decode) is **3,748 B**. The in-memory decoder tables are **35,168 B**: skeleton descriptors, rules, the entry table, and Huffman tables expanded to int64 values plus per-length counts.

## Frontier (estimate from the measured inputs)
The functions are ranked by self Ir. For k = 0..324, the hottest k chibicc functions stay native and the rest run from the payload. One guest instruction is one x86 instruction of the original code, so the cold functions' Ir counts guest instructions.
- `cold_Ir(k)` = the self Ir of the 324−k cold functions
- `hot_fraction(k) = 1 − cold_Ir(k) / total_Ir`, where total_Ir counts chibicc, libc and ld.so
- `est_ratio(k) = hot_fraction(k) + cold_Ir(k) × c / T_native`, with c = 9.66 ns (optimistic) or 3 × 9.66 = 28.98 ns (pessimistic)
- `resident(k) = payload 47,034 + native bytes of the k hot functions + symbol map 5,292 (E11's estimate) + decoder tables 35,168 + decoder .text 3,748`

The whole payload stays resident; it is not trimmed to the cold set. The fixed part is 91,242 B, or 34.6% of `.text`, which is the floor at k=0. At k=0 the ratio is 80.4× (optimistic) and 240.9× (pessimistic).

| cost | smallest k with ratio ≤1.5 | cold Ir (share of total) | resident | **fraction of native .text** |
|---|---|---|---|---|
| optimistic, 9.66 ns | 86 | 4.90 M (0.51%) | 190,807 B | **72.4%** |
| pessimistic, 28.98 ns | 121 | 1.60 M (0.17%) | 229,211 B | **86.9%** |

**Sensitivity (estimates, not part of the rule).** For the optimistic case:
- Dropping the symbol map, decoder tables and decoder code entirely leaves payload plus hot native at 55.6%, still above 50%.
- Also trimming the payload to the cold functions, pro rata by native bytes (29.3 KB), gives 48.9%, just under 50%.
- So the "stop" outcome depends on charging the decoder's resident state and on not trimming the payload.
- The wall time is also noisy. A larger T leaves more room for cold code, and at T = 0.1045 s (an earlier run) the optimistic result was 71.0%.

## Decision (plan rule)
The optimistic cost needs 72.4% > 50% resident, so the rule says **stop**: executable compression buys little RAM for this program. The pessimistic ≤30% branch is unreachable anyway, because the k=0 floor is already 34.6%.

## Scope and caveats
- The optimistic cost is a lower bound on interpretation cost: decode only, using a bit-serial canonical Huffman decoder. A lookup-table decoder could be faster, but it would also raise the table bytes.
- The order is by self Ir, as the plan says. Ordering by Ir per native byte was not tried.
- The single workload is tokenize.c, so the profile could shift on other inputs.
